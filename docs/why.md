## Why This Exists

This project began with a practical production-line requirement: replace a mini PC currently running Node-RED and Mosquitto with a compact, purpose-built embedded appliance.

The existing mini PC performs an important role, but it also brings the maintenance concerns of a general-purpose computer:

* Operating-system updates and unexpected restarts
* Storage corruption and disk failure
* Application, service, and dependency management
* Longer startup and recovery times
* Additional configuration required to ensure services restart correctly
* More hardware and software than the application actually requires

A NetBurner module can provide the required MQTT and machine-integration functions without a desktop operating system, container runtime, or separate broker computer. It can boot with the production equipment and operate as a dedicated component of the machine system.

### Broker at the Machine Edge

Placing the MQTT broker directly on the production line keeps machine-level messaging local. PLCs, embedded controllers, sensors, and other devices can communicate through the local broker without depending on a separate PC, cloud service, or continuously available plant server.

On NetBurner platforms with two Ethernet interfaces, the appliance can connect the production-line network to a separate plant or application network while keeping the two networks logically distinct.

One interface can serve the isolated machine network, while the second connects to systems that publish or consume MQTT data. Machine devices do not require direct access to the broader plant network. Instead, selected information is exchanged through the broker and through any application logic implemented in the firmware.

The two-interface design should not be interpreted as a general-purpose router, firewall, or transparent network bridge unless those functions are deliberately implemented and validated. The intended architecture is application-level separation: only MQTT and other explicitly enabled services are exposed on each interface.

This allows the appliance to act as a controlled application boundary between the machine and plant networks without automatically forwarding unrelated network traffic.

### Why NetBurner

NetBurner was selected because it is an established embedded networking platform rather than a general-purpose computer or consumer single-board computer.

The company has been developing embedded networking products since 1998, and its modules are designed specifically for applications that require reliable network connectivity, fast startup, embedded web configuration, diagnostics, and long-running unattended operation.

Using NetBurner provides several practical advantages for this project:

* A purpose-built embedded software and networking environment
* Fast and predictable startup
* No desktop operating system or package-management dependencies
* Integrated Ethernet and TCP/IP support
* Support for multiple network interfaces on applicable modules
* Embedded web configuration and diagnostics
* Field firmware-update capabilities
* Compact hardware suitable for integration into industrial equipment
* A product family that can support the same software architecture across multiple performance levels

The use of NetBurner does not, by itself, guarantee system reliability. Reliability still depends on the application firmware, power supply, enclosure, network design, watchdog behavior, update process, and validation of the finished appliance. However, it provides a mature and application-focused foundation for building a dedicated production-line device.

### Replacing the Existing Mini PC

The MQTT broker is the foundation of the appliance, but replacing the existing Node-RED and Mosquitto installation may also require application-specific firmware to perform the functions currently handled by Node-RED.

Depending on the production line, those functions may include:

* Communicating with PLCs and machine devices
* Publishing machine data over MQTT
* Subscribing to commands and production messages
* Transforming or routing data between protocols
* Logging production information to a database
* Buffering data during temporary network or server outages
* Providing a local web interface for configuration and diagnostics

This project is not intended to reproduce Node-RED as a general-purpose visual programming environment or compete with enterprise MQTT broker clusters. Its purpose is to replace a specific production-line mini PC with a fast-booting, locally managed embedded appliance containing only the functionality required by the machine.
