#include "ProjectManager.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        vyx::printVyxUsage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "build")              return vyx::cmdBuild(argc, argv);

    std::cerr << "unknown command: " << cmd << '\n';
    vyx::printVyxUsage(argv[0]);
    return 1;
}
