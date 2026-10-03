#include "LSPServer.h"

int main(int, char*[]) {
    vyx::lsp::LSPServer server;
    server.run();
    return 0;
}
