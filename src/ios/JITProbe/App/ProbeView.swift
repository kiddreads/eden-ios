// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One screen. A non-technical tester must be able to install this, open it, and read
// the answer without being told what mprotect is.
//
// The hard part of this screen is not layout, it is that THE APP MAY CLOSE ITSELF
// WHILE THE TESTER IS LOOKING AT IT, and that this is a successful outcome rather than
// a bug. So:
//   - the UI is painted and given a moment to render BEFORE each dangerous step, so a
//     tester who sees the app vanish knows which step it vanished on,
//   - the copy says, before anything happens, that closing is expected and that
//     reopening continues the test,
//   - a strategy that killed the process is never retried, so reopening always makes
//     progress and the app can never trap itself in a launch-crash loop.

import SwiftUI
import UIKit

// MARK: - model

@MainActor
final class ProbeModel: ObservableObject {

    struct Row: Identifiable {
        let id: Int
        let label: String
        let outcome: String
        let isPass: Bool
        let isFatal: Bool
        let isUntried: Bool
        let isDecisive: Bool
    }

    @Published private(set) var headline = "TESTING"
    @Published private(set) var rows: [Row] = []
    @Published private(set) var finished = false
    @Published private(set) var passed = false
    @Published private(set) var resumedAfterKill = false
    @Published private(set) var journalWritable = true
    @Published private(set) var debuggerPresent = false
    @Published private(set) var currentlyTesting: Int?
    @Published private(set) var report = ""

    private var started = false

    func refresh() {
        headline = String(cString: jp_headline())
        finished = jp_all_done()
        passed = jp_any_pass()
        resumedAfterKill = jp_any_killed()
        journalWritable = jp_journal_writable()
        debuggerPresent = jp_cs_debugged()

        let total = Int(jp_strategy_total())
        rows = (0..<total).map { index in
            Row(id: index,
                label: String(cString: jp_strategy_label(Int32(index))),
                outcome: String(cString: jp_strategy_outcome_text(Int32(index))),
                isPass: jp_strategy_is_pass(Int32(index)),
                isFatal: jp_strategy_is_fatal(Int32(index)),
                isUntried: jp_strategy_is_untried(Int32(index)),
                isDecisive: jp_strategy_is_decisive(Int32(index)))
        }

        var buffer = [CChar](repeating: 0, count: 16384)
        buffer.withUnsafeMutableBufferPointer { raw in
            jp_format_report(raw.baseAddress, raw.count)
        }
        report = String(cString: buffer)

        persistReport()
    }

    /// Writes the report next to the journal, so the footnote at the bottom of the
    /// screen is true. It matters more than it looks: if the device refuses JIT
    /// fatally enough that the app cannot stay open long enough to be read, the file
    /// in Documents is how the answer gets off the device.
    private func persistReport() {
        guard let documents = FileManager.default.urls(for: .documentDirectory,
                                                       in: .userDomainMask).first else {
            return
        }
        let url = documents.appendingPathComponent("jit-probe-report.txt")
        try? report.write(to: url, atomically: true, encoding: .utf8)
    }

    /// Reads the environment and replays any previous launch. Executes nothing.
    func begin() {
        _ = jp_current_report()
        refresh()
    }

    /// Runs the remaining strategies, one at a time, yielding to the run loop between
    /// each so the screen is up to date before the next one is attempted.
    func runAll() async {
        guard !started else { return }
        started = true

        while !jp_all_done() {
            let next = Int(jp_next_strategy())
            currentlyTesting = next
            refresh()

            // Let SwiftUI actually draw the "testing N" state. If the process is about
            // to be killed, this is the difference between a tester who can say which
            // step it died on and one who can only say "it closed".
            try? await Task.sleep(nanoseconds: 400_000_000)

            // THE CALL THAT MAY NOT RETURN.
            _ = jp_run_next_strategy()

            currentlyTesting = nil
            refresh()
            try? await Task.sleep(nanoseconds: 150_000_000)
        }
        currentlyTesting = nil
        refresh()
    }

    func startOver() {
        jp_reset()
        started = false
        refresh()
        Task { await runAll() }
    }

    func copyReport() {
        UIPasteboard.general.string = report
    }
}

// MARK: - screen

struct ProbeView: View {

    @StateObject private var model = ProbeModel()
    @State private var showRawReport = false
    @State private var copied = false

    var body: some View {
        NavigationView {
            ScrollView {
                content
                    .padding(20)
            }
            .navigationTitle("Eden JIT probe")
            .navigationBarTitleDisplayMode(.inline)
        }
        .navigationViewStyle(.stack)
        .task {
            model.begin()
            await model.runAll()
        }
    }

    // Split out of `body`, and split again into halves below, because Swift's type
    // checker gave up on the original single expression:
    //   "the compiler is unable to type-check this expression in reasonable time"
    // Seven heterogeneous sub-views plus two conditional branches in one VStack is
    // enough to make inference blow up. Each @ViewBuilder property gets its own
    // annotated result type, so the checker never has to solve the whole tree at once.
    @ViewBuilder
    private var content: some View {
        VStack(alignment: .leading, spacing: 22) {
            topSection
            bottomSection
        }
    }

    @ViewBuilder
    private var topSection: some View {
        VStack(alignment: .leading, spacing: 22) {
            verdictBanner
            meaning
            if model.resumedAfterKill {
                killNotice
            }
            if !model.journalWritable {
                journalWarning
            }
        }
    }

    @ViewBuilder
    private var bottomSection: some View {
        VStack(alignment: .leading, spacing: 22) {
            strategyList
            actions
            footnote
        }
    }

    // MARK: banner

    private var bannerColour: Color {
        if !model.finished { return .orange }
        return model.passed ? .green : .red
    }

    private var verdictBanner: some View {
        VStack(spacing: 10) {
            Text(model.finished ? (model.passed ? "PASS" : "FAIL") : "TESTING…")
                .font(.system(size: 64, weight: .heavy, design: .rounded))
                .foregroundColor(.white)
                .frame(maxWidth: .infinity)
            Text(model.finished
                 ? (model.passed ? "This device can run Eden's recompiler."
                                 : "This device cannot run Eden at all.")
                 : "Leave this open. It takes a few seconds.")
                .font(.headline)
                .foregroundColor(.white.opacity(0.95))
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
        }
        .padding(.vertical, 28)
        .background(bannerColour)
        .cornerRadius(18)
    }

    // MARK: plain English

    // The three explanation strings used to be built INSIDE the ViewBuilder via an
    // if/else-if/else with multiline literals - that combination (heavy string
    // literals plus branching, all inside view-builder type inference) is exactly
    // what made Swift give up with "unable to type-check this expression in
    // reasonable time". Precomputing plain Strings outside any ViewBuilder context
    // removes the string/branch assembly from the inference problem entirely; the
    // view body is then just one Text(constant) per branch, chosen by a switch.
    private var meaningText: String {
        if !model.finished {
            return """
                   This app writes four machine instructions into memory and tries to \
                   run them. Nothing else. It is checking the one thing Eden cannot \
                   work without.

                   If the app closes by itself, that is a result, not a crash — it \
                   means iOS refused. Just open it again and it will carry on from \
                   where it stopped. You may have to do that up to four times.
                   """
        } else if model.passed {
            return """
                   The app wrote instructions into memory and ran them, and they \
                   returned the right answers. That is the permission Eden's \
                   recompiler needs.

                   This result is about THIS device with THIS app installed THIS way. \
                   Installing differently — or, if a JIT enabler was used, not using \
                   it next time — can change it.
                   """
        } else {
            return """
                   Every way of getting runnable memory was refused or killed the app.

                   Eden has no fallback for this. It has no interpreter, so this is \
                   not "Eden would be slow here" — it is that Eden cannot emulate on \
                   this device, installed this way, at all.

                   If you have a JIT enabler such as StikDebug or a TrollStore \
                   install, launching through it and running this again is the next \
                   thing to try.
                   """
        }
    }

    private var debuggerLabelText: String {
        "A debugger is attached (CS_DEBUGGED is set), so this result is for the debugger-attached case."
    }

    private var meaning: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("What this is")
                .font(.headline)
            Text(meaningText)
                .font(.callout)
            if model.debuggerPresent {
                Label(debuggerLabelText, systemImage: "ladybug")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
        }
    }

    private var killNotice: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("The app closed itself earlier", systemImage: "exclamationmark.triangle.fill")
                .font(.headline)
                .foregroundColor(.orange)
            Text("""
                 That was iOS refusing to run the instructions, and it was recorded \
                 before it happened. The step it died on is marked below and will not \
                 be tried again.
                 """)
                .font(.footnote)
        }
        .padding(14)
        .background(Color.orange.opacity(0.12))
        .cornerRadius(12)
    }

    private var journalWarning: some View {
        VStack(alignment: .leading, spacing: 6) {
            Label("Cannot write to Documents", systemImage: "externaldrive.badge.xmark")
                .font(.headline)
                .foregroundColor(.red)
            Text("""
                 If the app closes itself, nothing will have been saved and the same \
                 step will simply be tried again on the next launch. Report this — it \
                 means the result below may be incomplete.
                 """)
                .font(.footnote)
        }
        .padding(14)
        .background(Color.red.opacity(0.12))
        .cornerRadius(12)
    }

    // MARK: steps

    private var strategyList: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("The four ways of asking")
                .font(.headline)
            ForEach(model.rows) { row in
                HStack(alignment: .top, spacing: 12) {
                    icon(for: row)
                        .frame(width: 24)
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 6) {
                            Text(row.label)
                                .font(.subheadline.weight(.semibold))
                            if row.isDecisive {
                                Text("THE ONE THAT COUNTS")
                                    .font(.caption2.weight(.bold))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(Color.accentColor.opacity(0.18))
                                    .cornerRadius(4)
                            }
                        }
                        Text(model.currentlyTesting == row.id ? "testing now…" : row.outcome)
                            .font(.footnote)
                            .foregroundColor(.secondary)
                    }
                    Spacer(minLength: 0)
                }
            }
        }
    }

    @ViewBuilder
    private func icon(for row: ProbeModel.Row) -> some View {
        if model.currentlyTesting == row.id {
            ProgressView()
        } else if row.isPass {
            Image(systemName: "checkmark.circle.fill").foregroundColor(.green)
        } else if row.isFatal {
            Image(systemName: "xmark.octagon.fill").foregroundColor(.red)
        } else if row.isUntried {
            Image(systemName: "circle.dashed").foregroundColor(.secondary)
        } else {
            Image(systemName: "minus.circle.fill").foregroundColor(.orange)
        }
    }

    // MARK: buttons

    private var actions: some View {
        VStack(spacing: 12) {
            Button {
                model.copyReport()
                copied = true
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) { copied = false }
            } label: {
                Label(copied ? "Copied" : "Copy the full report",
                      systemImage: copied ? "checkmark" : "doc.on.doc")
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
            }
            .background(Color.accentColor)
            .foregroundColor(.white)
            .cornerRadius(12)

            Button {
                showRawReport.toggle()
            } label: {
                Text(showRawReport ? "Hide the details" : "Show the details")
                    .frame(maxWidth: .infinity)
            }

            if showRawReport {
                // .textSelection is iOS 15, which is this project's deployment floor
                // (src/ios/project.yml, README.md "Devices"). Selectable as well as
                // copyable so a tester can send one line rather than all of it.
                Text(model.report)
                    .font(.system(size: 11, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(12)
                    .background(Color.secondary.opacity(0.10))
                    .cornerRadius(10)
            }

            if model.finished {
                Button("Run the whole thing again", role: .destructive) {
                    model.startOver()
                }
                .font(.footnote)
            }
        }
    }

    private var footnote: some View {
        Text("""
             The full report is also saved in this app's Documents folder, which you \
             can reach from the Files app. Nothing is sent anywhere; copying and pasting \
             it is the only way it leaves the device.
             """)
            .font(.caption)
            .foregroundColor(.secondary)
    }
}
