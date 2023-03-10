#include "messagedirector/message_director.h"
#include "util/config.h"
#include "util/globals.h"
#include "util/logger.h"
#include "util/metrics.h"

using namespace Ardos;

int main(int argc, char *argv[]) {
  // Parse CLI args.
  // We only have one for now, which is our config file name.
  std::string configName = "config.yml";
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      configName = argv[++i];
    }
  }

  Config::Instance()->LoadConfig(configName);

  Logger::SetLogLevel(Config::Instance()->GetString("log-level", "warning"));

  Logger::Info("Starting Ardos cluster...");

  // Load DC files from config.
  g_dc_file = new DCFile();

  auto dcList = Config::Instance()->GetNode("dc-files");
  if (!dcList) {
    Logger::Error("Your config file must contain a dc-files definition!");
    return EXIT_FAILURE;
  }

  auto dcNames = dcList.as<std::vector<std::string>>();
  for (auto dcName : dcNames) {
    if (!g_dc_file->read(dcName)) {
      // Just die if we can't read a DC file, they're very important to have
      // loaded correctly.
      Logger::Error(std::format("Failed to read DC file `{}`!", dcName));
      return EXIT_FAILURE;
    }
  }

  Logger::Verbose(std::format("Computed DC hash: {}", g_dc_file->get_hash()));

  // Setup main event loop.
  g_main_thread_id = std::this_thread::get_id();
  g_loop = uvw::Loop::getDefault();

  // Initialize Metrics (Prometheus).
  // Metrics can be configured via the config file.
  Metrics::Instance();

  // Initialize the Message Director.
  // This will automatically start up configured roles once a connection to
  // RabbitMQ is made.
  MessageDirector::Instance();

  g_loop->run();

  return EXIT_SUCCESS;
}
