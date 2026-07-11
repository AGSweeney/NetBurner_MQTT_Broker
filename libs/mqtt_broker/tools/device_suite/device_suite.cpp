// mqtt_device_suite — live device conformance EXE (no Python/Node).
#include "suite_impl.hpp"

int main(int argc, char **argv)
{
    return device_suite::suite::run_suite(argc, argv);
}
