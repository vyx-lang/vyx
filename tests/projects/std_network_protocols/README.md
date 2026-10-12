# Network protocol consumer

This project imports the real `std:network` source package. Its AOT executable
checks HTTP, unary gRPC and text WebSocket communication against local services.

Run the [network protocol gate](../../../probes/gates/network-protocols/README.md)
with the current-source SDK compiler and native dependencies. The gate supplies
ephemeral server endpoints and a test CA, builds the project and checks server
observations as well as the executable's exit status. Running the executable
alone requires those services and environment variables.

The repository test runner calls `run.ps1`. Set `VYX_NETWORK_NATIVE_BIN` to a
platform path-separated list of native library directories, and optionally
`VYX_NETWORK_PYTHON_PACKAGES` to an isolated Python package directory.
