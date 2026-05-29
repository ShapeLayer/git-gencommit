#!/usr/bin/env bash
set -euo pipefail

# Build and install git-gencommit into ~/bin as a git subcommand.
# After install, `git gencommit` resolves to ~/bin/git-gencommit.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BIN_DIR="$HOME/bin"
STARTUP_SCRIPT="$BIN_DIR/git-gencommit-env.sh"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --config Release -j

mkdir -p "$BIN_DIR"
cp "$BUILD_DIR/git-gencommit" "$BIN_DIR/git-gencommit"
chmod +x "$BIN_DIR/git-gencommit"

cat > "$STARTUP_SCRIPT" <<'EOF'
#!/usr/bin/env bash
export PATH="$HOME/bin:$PATH"
EOF
chmod +x "$STARTUP_SCRIPT"

echo "Installed: $BIN_DIR/git-gencommit"
echo "Created startup script: $STARTUP_SCRIPT"
echo "Add this line to your shell profile (~/.zshrc, ~/.bashrc, etc.):"
echo "  source ~/bin/git-gencommit-env.sh"
