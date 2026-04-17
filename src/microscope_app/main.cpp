#include <QApplication>
#include <csignal>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // Enable Ctrl+C termination
    std::signal(SIGINT, SIG_DFL);

    QApplication app(argc, argv);

    MainWindow window;
    window.showMaximized();

    return app.exec();
}
