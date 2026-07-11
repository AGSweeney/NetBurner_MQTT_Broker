// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Dashboard Chart.js views fed by WebSocket /ws/stats.

var statsWs = null;
var statsWsRetryTimer = null;
var chartHistoryLen = 60;
var prevPublishRx = null;
var prevPublishTx = null;
var prevConnectAccept = null;
var prevConnectReject = null;
var prevInterfaceCounters = {};

var chartClients = null;
var chartPublish = null;
var chartIfThroughput = null;
var chartConnect = null;

var nbChartColors = {
  red: "#DC1F26",
  carbon: "#111418",
  gray500: "#7C8590",
  green: "#2E7D5B",
  amber: "#C77A12",
  blue: "#0B6FB4"
};

function formatUptimeLabel(secs) {
  var total = Number(secs) || 0;
  var minutes = Math.floor(total / 60);
  var seconds = total % 60;
  return minutes + ":" + (seconds < 10 ? "0" : "") + seconds;
}

function lineDataset(label, color, fillColor) {
  return {
    label: label,
    data: [],
    borderColor: color,
    backgroundColor: fillColor || undefined,
    fill: !!fillColor,
    borderWidth: 2,
    tension: 0.2,
    pointRadius: 0,
    pointHoverRadius: 3,
    spanGaps: true
  };
}

function pushChartPoint(chart, label, values) {
  chart.data.labels.push(label);
  for (var i = 0; i < values.length; i++) {
    chart.data.datasets[i].data.push(values[i]);
  }
  while (chart.data.labels.length > chartHistoryLen) {
    chart.data.labels.shift();
    for (var j = 0; j < chart.data.datasets.length; j++) {
      chart.data.datasets[j].data.shift();
    }
  }
}

function refreshChart(chart) {
  chart.data.labels = chart.data.labels.slice();
  for (var i = 0; i < chart.data.datasets.length; i++) {
    chart.data.datasets[i].data = chart.data.datasets[i].data.slice();
  }
  chart.update();
}

function setWsStatus(cls, text) {
  var el = document.getElementById("wsStatusPill");
  if (el) {
    el.className = "nb-status " + cls;
    el.textContent = text;
  }
}

function integerTick(value) {
  return Number.isInteger(value) ? value : "";
}

function throughputTick(value) {
  if (value === 0) {
    return "0";
  }
  if (value >= 100) {
    return Math.round(value);
  }
  if (value >= 10) {
    return value.toFixed(1);
  }
  return value.toFixed(2);
}

function yScaleOptions(kind, title) {
  var scale = {
    type: "linear",
    beginAtZero: true,
    title: title ? { display: true, text: title, font: { size: 11 } } : undefined,
    grid: { color: "rgba(17,20,24,0.06)" }
  };

  if (kind === "throughput") {
    scale.grace = "5%";
    scale.ticks = { font: { size: 10 }, callback: throughputTick };
    return scale;
  }

  scale.grace = 0;
  scale.ticks = {
    font: { size: 10 },
    precision: 0,
    stepSize: 1,
    callback: integerTick
  };
  return scale;
}

function chartMaxValue(chart) {
  var maxVal = 0;
  for (var d = 0; d < chart.data.datasets.length; d++) {
    var data = chart.data.datasets[d].data;
    for (var i = 0; i < data.length; i++) {
      var v = data[i];
      if (v != null && v > maxVal) {
        maxVal = v;
      }
    }
  }
  return maxVal;
}

function tuneIntegerYAxis(chart, minMax) {
  var maxVal = chartMaxValue(chart);
  chart.options.scales.y.suggestedMax = Math.max(minMax || 1, Math.ceil(maxVal));
  chart.options.scales.y.ticks.stepSize = 1;
}

function tuneThroughputYAxis(chart) {
  var maxVal = chartMaxValue(chart);
  if (maxVal <= 0) {
    chart.options.scales.y.suggestedMax = 10;
    return;
  }
  if (maxVal < 1) {
    chart.options.scales.y.suggestedMax = Math.ceil(maxVal * 10) / 10 + 0.1;
    return;
  }
  if (maxVal < 10) {
    chart.options.scales.y.suggestedMax = Math.ceil(maxVal);
    return;
  }
  chart.options.scales.y.suggestedMax = Math.ceil(maxVal * 1.1);
}

function chartBaseOptions(yTitle, yKind) {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    interaction: { mode: "index", intersect: false },
    plugins: {
      legend: { position: "bottom", labels: { boxWidth: 12, font: { size: 11 } } }
    },
    scales: {
      x: {
        type: "category",
        ticks: { maxTicksLimit: 6, font: { size: 10 }, maxRotation: 0 },
        grid: { color: "rgba(17,20,24,0.06)" }
      },
      y: yScaleOptions(yKind || "integer", yTitle)
    }
  };
}

function initDashboardCharts() {
  if (typeof Chart === "undefined") {
    setWsStatus("nb-status-err", "Chart.js unavailable");
    return;
  }

  chartClients = new Chart(document.getElementById("chartClients"), {
    type: "line",
    data: {
      labels: [],
      datasets: [
        lineDataset("Connected", nbChartColors.red, "rgba(220,31,38,0.08)"),
        lineDataset("TLS", nbChartColors.blue, null)
      ]
    },
    options: chartBaseOptions("clients", "integer")
  });

  chartPublish = new Chart(document.getElementById("chartPublish"), {
    type: "line",
    data: {
      labels: [],
      datasets: [
        lineDataset("RX / 2s", nbChartColors.green, null),
        lineDataset("TX / 2s", nbChartColors.amber, null)
      ]
    },
    options: chartBaseOptions("messages / 2s", "integer")
  });

  chartIfThroughput = new Chart(document.getElementById("chartIfThroughput"), {
    type: "line",
    data: {
      labels: [],
      datasets: []
    },
    options: chartBaseOptions("kbps", "throughput")
  });

  chartConnect = new Chart(document.getElementById("chartConnect"), {
    type: "line",
    data: {
      labels: [],
      datasets: [
        lineDataset("Accept / 2s", nbChartColors.green, null),
        lineDataset("Reject / 2s", nbChartColors.red, null)
      ]
    },
    options: chartBaseOptions("events / 2s", "integer")
  });

  window.requestAnimationFrame(function () {
    refreshChart(chartClients);
    refreshChart(chartPublish);
    refreshChart(chartIfThroughput);
    refreshChart(chartConnect);
  });

  connectStatsWebSocket();
}

function wsUrl() {
  var prefix = (location.protocol === "https:") ? "wss://" : "ws://";
  var host = window.location.hostname;
  var port = (window.location.port !== "") ? (":" + window.location.port) : "";
  return prefix + host + port + "/ws/stats";
}

function resetRateBaselines() {
  prevPublishRx = null;
  prevPublishTx = null;
  prevConnectAccept = null;
  prevConnectReject = null;
  prevInterfaceCounters = {};
}

function connectStatsWebSocket() {
  if (!("WebSocket" in window)) {
    setWsStatus("nb-status-err", "WebSocket not supported");
    return;
  }

  if (statsWs && (statsWs.readyState === WebSocket.OPEN || statsWs.readyState === WebSocket.CONNECTING)) {
    return;
  }

  setWsStatus("nb-status-info", "WebSocket connecting");
  statsWs = new WebSocket(wsUrl());

  statsWs.onopen = function () {
    setWsStatus("nb-status-ok", "WebSocket live");
    resetRateBaselines();
    if (statsWsRetryTimer) {
      clearTimeout(statsWsRetryTimer);
      statsWsRetryTimer = null;
    }
  };

  statsWs.onmessage = function (evt) {
    try {
      var payload = JSON.parse(String(evt.data).trim());
      applyStatsPayload(payload);
    } catch (e) {
      setWsStatus("nb-status-warn", "Bad stats payload");
    }
  };

  statsWs.onclose = function () {
    setWsStatus("nb-status-warn", "WebSocket disconnected — retrying");
    statsWs = null;
    statsWsRetryTimer = setTimeout(connectStatsWebSocket, 3000);
  };

  statsWs.onerror = function () {
    setWsStatus("nb-status-err", "WebSocket error");
  };
}

function applyStatsPayload(payload) {
  var broker = payload.broker || {};
  var interfaces = payload.interfaces || [];
  var label = formatUptimeLabel(payload.ts);

  if (payload.platform && payload.uptime !== undefined) {
    document.getElementById("headerMeta").textContent =
      payload.platform + " · uptime " + payload.uptime + "s";
  }

  if (chartClients) {
    pushChartPoint(chartClients, label, [
      Math.round(broker.clients_connected || 0),
      Math.round(broker.tls_clients_connected || 0)
    ]);
    tuneIntegerYAxis(chartClients, 1);
    refreshChart(chartClients);
  }

  if (chartPublish) {
    var rx = broker.publish_received || 0;
    var tx = broker.publish_sent || 0;
    var rxRate = (prevPublishRx === null) ? 0 : Math.max(0, rx - prevPublishRx);
    var txRate = (prevPublishTx === null) ? 0 : Math.max(0, tx - prevPublishTx);
    prevPublishRx = rx;
    prevPublishTx = tx;
    pushChartPoint(chartPublish, label, [rxRate, txRate]);
    tuneIntegerYAxis(chartPublish, 1);
    refreshChart(chartPublish);
  }

  if (chartConnect) {
    var accept = broker.connect_accept || 0;
    var reject = broker.connect_reject || 0;
    var acceptRate = (prevConnectAccept === null) ? 0 : Math.max(0, accept - prevConnectAccept);
    var rejectRate = (prevConnectReject === null) ? 0 : Math.max(0, reject - prevConnectReject);
    prevConnectAccept = accept;
    prevConnectReject = reject;
    pushChartPoint(chartConnect, label, [acceptRate, rejectRate]);
    tuneIntegerYAxis(chartConnect, 1);
    refreshChart(chartConnect);
  }

  if (chartIfThroughput) {
    updateInterfaceThroughput(interfaces, label, Number(payload.ts) || 0);
  }

  updateStatusTilesFromBroker(broker);
}

function counterDelta(current, previous) {
  if (current >= previous) {
    return current - previous;
  }
  return (0x100000000 - previous) + current;
}

function interfaceDataset(key, label, color, pointCount) {
  return {
    interfaceKey: key,
    label: label,
    data: new Array(pointCount).fill(null),
    borderColor: color,
    borderWidth: 2,
    tension: 0.2,
    pointRadius: 0,
    pointHoverRadius: 3,
    spanGaps: true
  };
}

function findInterfaceDataset(key) {
  for (var i = 0; i < chartIfThroughput.data.datasets.length; i++) {
    if (chartIfThroughput.data.datasets[i].interfaceKey === key) {
      return chartIfThroughput.data.datasets[i];
    }
  }
  return null;
}

function updateInterfaceThroughput(interfaces, label, timestamp) {
  chartIfThroughput.data.labels.push(label);
  chartIfThroughput.data.datasets.forEach(function (dataset) {
    dataset.data.push(null);
  });

  interfaces.forEach(function (iface, index) {
    var id = String(iface.id);
    var name = iface.name || ("if" + id);
    var rxKey = id + ":rx";
    var txKey = id + ":tx";
    var rxDataset = findInterfaceDataset(rxKey);
    var txDataset = findInterfaceDataset(txKey);
    var pointCount = chartIfThroughput.data.labels.length;

    if (!rxDataset) {
      rxDataset = interfaceDataset(
        rxKey, name + " RX", index % 2 ? nbChartColors.blue : nbChartColors.green, pointCount
      );
      chartIfThroughput.data.datasets.push(rxDataset);
    }
    if (!txDataset) {
      txDataset = interfaceDataset(
        txKey, name + " TX", index % 2 ? nbChartColors.red : nbChartColors.amber, pointCount
      );
      chartIfThroughput.data.datasets.push(txDataset);
    }

    var currentRx = Number(iface.rx_bytes) || 0;
    var currentTx = Number(iface.tx_bytes) || 0;
    var previous = prevInterfaceCounters[id];
    var rxKbps = 0;
    var txKbps = 0;
    if (previous && timestamp > previous.timestamp) {
      var elapsed = timestamp - previous.timestamp;
    rxKbps = counterDelta(currentRx, previous.rx) * 8 / elapsed / 1000;
    txKbps = counterDelta(currentTx, previous.tx) * 8 / elapsed / 1000;
    }
    rxDataset.data[pointCount - 1] = Math.round(rxKbps * 100) / 100;
    txDataset.data[pointCount - 1] = Math.round(txKbps * 100) / 100;
    prevInterfaceCounters[id] = { rx: currentRx, tx: currentTx, timestamp: timestamp };
  });

  while (chartIfThroughput.data.labels.length > chartHistoryLen) {
    chartIfThroughput.data.labels.shift();
    chartIfThroughput.data.datasets.forEach(function (dataset) {
      dataset.data.shift();
    });
  }
  tuneThroughputYAxis(chartIfThroughput);
  refreshChart(chartIfThroughput);
}

function updateStatusTilesFromBroker(broker) {
  document.getElementById("statClients").textContent =
    (broker.clients_connected || 0) + " connected" +
    (broker.tls_clients_connected ? " · " + broker.tls_clients_connected + " TLS" : "");

  document.getElementById("statPublish").textContent =
    (broker.publish_received || 0) + " / " + (broker.publish_sent || 0);

  var parts = [];
  if (broker.plain_tcp_enabled) {
    parts.push("TCP :" + broker.plain_tcp_port + (broker.listener_active ? " active" : " inactive"));
  } else {
    parts.push("TCP disabled");
  }
  if (broker.tls_enabled) {
    parts.push("TLS :" + broker.tls_port + (broker.tls_listener_active ? " active" : " inactive"));
  } else {
    parts.push("TLS disabled");
  }
  document.getElementById("statListener").textContent = parts.join(" · ");

  var pill = document.getElementById("statusPill");
  var detail = document.getElementById("statusDetail");
  var anyActive = broker.listener_active || broker.tls_listener_active;
  pill.className = "nb-status " + (anyActive ? "nb-status-ok" : "nb-status-warn");
  pill.textContent = anyActive ? "Running" : "Listeners off";
  detail.textContent =
    "accept " + (broker.connect_accept || 0) +
    " · reject " + (broker.connect_reject || 0) +
    " · parser errors " + (broker.parser_errors || 0) +
    " · slow consumer drops " + (broker.slow_consumer_disconnects || 0);
}
