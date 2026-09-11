// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole app. One window, one screen, one question.
//
// WHY THE STATE DIRECTORY IS SET BEFORE ANYTHING ELSE HAPPENS
// ----------------------------------------------------------
// The probe may not survive its own test - that is the point of it. Its only way to
// report an uncatchable kill is a file that was already fsync()ed to disk when the
// process stopped existing. Setting the directory has to happen before the first call
// into the probe, so it is done in init(), not in a .task or .onAppear.
//
// WHY NOTHING IS PROBED FROM init()
// ---------------------------------
// Setting a path is not probing. docs/IOS_PORT_NOTES.md: "JIT memory is no longer
// allocated during static initialisation - SpinLockImpl was a namespace-scope global
// whose constructor mmapped executable memory at dyld load, before main(), so a device
// without JIT permission killed the app before any UI could explain why." An app that
// dies before it can draw the reason is exactly the failure this app exists to avoid
// reproducing.

import SwiftUI
import UIKit

@main
struct JITProbeApp: App {

    init() {
        // Documents, not a cache or a temporary directory. Two reasons, both real:
        // the journal must survive a process kill and a relaunch, and Info.plist sets
        // UIFileSharingEnabled so a tester can hand over the raw files through Files.app
        // when the in-app copy button is not enough.
        if let documents = FileManager.default.urls(for: .documentDirectory,
                                                    in: .userDomainMask).first {
            documents.path.withCString { jp_set_state_directory($0) }
        }

        // sysctl kern.osproductversion normally answers this; the hint is only used if
        // it does not, so that a pasted report never has a blank OS version in it.
        UIDevice.current.systemVersion.withCString { jp_set_os_version_hint($0) }
    }

    var body: some Scene {
        WindowGroup {
            ProbeView()
        }
    }
}
