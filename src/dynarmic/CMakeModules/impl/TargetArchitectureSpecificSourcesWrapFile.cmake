# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Do NOT uppercase. DetectArchitecture.cmake defines ARCHITECTURE_<arch> using the
# architecture name exactly as written - ARCHITECTURE_arm64, ARCHITECTURE_x86_64 - and the
# callers pass those same lowercase names. Uppercasing produced ARCHITECTURE_ARM64, which
# is never defined, so every wrapped source compiled to an empty translation unit and the
# resulting archive linked cleanly with no backend in it at all.
file(READ "${input_file}" f_contents)
file(WRITE "${output_file}" "#if defined(ARCHITECTURE_${arch})\n${f_contents}\n#endif\n")
