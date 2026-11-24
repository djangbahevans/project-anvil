#pragma once

#include "anvil/exthost/session.hpp"

#include <deque>
#include <string>

namespace anvil::exthost {

// `ActivationKind` of the extension host: `normal` waits for the extensions that the event
// activates, `immediate` is for events that must not wait for the startup activations.
enum class ActivationKind { normal = 0, immediate = 1 };

// The activation events that the editor fires (`ExtHostExtensionService.$activateByEvent`). The
// extension host fires some events itself, such as `*`, `onStartupFinished` and
// `workspaceContains:<file name>`; the editor fires `onCommand:<id>`, `onFileSystem:<scheme>` and
// the like, and learns when the activation has finished.
//
// The extension host answers `$activateByEvent` only once it has the workspace, and activates the
// extensions of `*` before the editor has told it about the documents, so an event fired earlier
// waits in a queue. set_ready() sends the queue in the order it was fired. Waiting for the end of
// the startup sequence instead would hold an event back for as long as an extension that is
// activated at startup waits for it, such as a command of another extension.
//
// The session must outlive the service.
class ActivationService {
public:
    explicit ActivationService(Session& session) : session_(session) {}

    // Fires `event`; `done` receives the reply of the extension host, or the reason that no reply
    // will come (fail_queued).
    void fire(std::string event, ActivationKind kind, Session::ReplyHandler done);

    // Sends the events that wait, and sends later events at once.
    void set_ready();

    // Completes the events that still wait with `reason` as their error.
    void fail_queued(const std::string& reason);

private:
    struct Queued {
        std::string event;
        ActivationKind kind;
        Session::ReplyHandler done;
    };
    void send(Queued queued);

    Session& session_;
    bool ready_ = false;
    std::deque<Queued> queue_;
};

} // namespace anvil::exthost
