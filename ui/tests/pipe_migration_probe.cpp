#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>
#include "pipe_client.hpp"
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    libs::ui::PipeClient client(QString::fromUtf8(argv[1]));
    try {
        if (!client.command_payload("ping").value("pong").toBool()) return 3;
        const auto status = client.command_payload("status");
        if (!status.contains("gate")) return 4;
        const auto sub = client.command_payload("spectrum_stream", {{"action", "subscribe"}});
        const auto id = sub.value("subscriber");
        const auto frame = client.command_payload("spectrum_stream", {{"action", "latest"}, {"subscriber", id}});
        if (frame.value("has_frame").toBool()) return 5;
        client.command_payload("spectrum_stream", {{"action", "unsubscribe"}, {"subscriber", id}});
        bool rejected = false;
        try { client.command("unknown_probe_command"); } catch (const QString&) { rejected = true; }
        if (!rejected) return 6;
        std::puts("Qt pipe client: ping/status/subscribe/latest/unsubscribe/error passed");
        return 0;
    } catch (const QString& e) { std::fprintf(stderr, "%s\n", qPrintable(e)); return 1; }
}
