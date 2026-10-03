#include "DAPServer.h"

int main(int, char*[]) {
    vyx::dap::DAPServer server;
    server.run();
    return 0;
}
