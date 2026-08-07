# Workspace Rules

- Only edit C++ files (`.cc`, `.h`, `.mm`, `.cpp`, `.hpp`) directly.
- If changes or additions are required in non-C++ files (such as `.vcxproj`, `.py`, build scripts, or project configurations) for support, do NOT edit them directly; inform the user clearly with the exact lines or files to update.
- After making code changes, thoroughly review and audit all modified files and execution paths to verify thread safety, boundary checks, and overall runtime stability.
