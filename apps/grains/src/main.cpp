// grains: labels grains in microscope video from Becke fringe motion.
//
//   grains replay <folder>            headless: runs a recording again, prints its summary;
//                                     exit 1 on an error, 3 when a report differs from the recorded one
//   grains --demo                     window on a synthetic scene; no config, no camera
//   grains [--config grains.toml]     window on the camera of the config
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>

#include "demo.hpp"
#include "grains_window.hpp"
#include "pychron/grains/config.hpp"
#include "live_poll.hpp"
#include "replay_command.hpp"
#include "pychron/vision/camera_backend.hpp"
#include "pychron/vision/live_feed.hpp"

int main(int argc, char** argv) {
  // Before any QApplication: a replay needs no display.
  if (argc >= 2 && std::string_view(argv[1]) == "replay") {
    if (argc != 3) {
      std::cerr << "usage: grains replay <folder>\n";
      return 2;
    }
    return pychron::grains::run_replay(argv[2], std::cout, std::cerr);
  }

  bool demo = false;
  std::string config_path = "./grains.toml";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--demo") {
      demo = true;
    } else if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      std::cerr << "usage: grains [--demo | --config <grains.toml>] | grains replay <folder>\n";
      return 2;
    }
  }

  QCoreApplication::setOrganizationName(QStringLiteral("pychron"));
  QCoreApplication::setApplicationName(QStringLiteral("grains"));
  QApplication app(argc, argv);

  if (demo) {
    pychron::grains::GrainsWindow window(pychron::grains::demo_poll(), pychron::grains::GrainOptions{});
    window.show_notice(QObject::tr("demo: use the reference tool on the grain at the lower right"));
    window.resize(1000, 640);
    window.show();
    return QApplication::exec();
  }

  const auto config = pychron::grains::load_config(config_path);
  if (!config) {
    QMessageBox::critical(nullptr, QObject::tr("grains"),
                          QObject::tr("%1: %2").arg(QString::fromStdString(config_path), QString::fromStdString(config.error().what)));
    return 1;
  }
  // The feed outlives the window: the window's worker polls it.
  pychron::vision::LiveFeed feed([request = config->camera](pychron::vision::ClockFn stamp) {
    return pychron::vision::open_camera(request, std::move(stamp));
  });
  pychron::grains::GrainsWindow window(
      pychron::grains::live_poll(feed), config->pipeline);
  window.resize(1000, 640);
  window.show();
  return QApplication::exec();
}
