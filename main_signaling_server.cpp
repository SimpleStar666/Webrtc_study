#include "signaling/signaling_server.h"
#include "utils/logger.h"

int main(int argc, char* argv[]) {
    crystal::Logger::init("signaling_server");
    crystal::SignalingServer server("0.0.0.0", 8765);
    server.start();
    return 0;
}
