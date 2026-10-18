# Claude Cardputer Bridge

An optional local Node.js service used by the Claude screens in the Cardputer
firmware. It invokes the existing Claude Code CLI login on this computer; it
does not require a separate Anthropic API key.

## Setup

From the repository root, copy `.bridge-token.example.ps1` to
`.bridge-token.ps1` and set a long random token. Set the exact same token in
`include/pocket_claude_secrets.h` in the firmware project. Then run:

```powershell
claude-cardputer-bridge/start-bridge.ps1
```

The launcher binds to all network interfaces so the Cardputer can reach it.
Allow access only on trusted private networks in Windows Firewall. Do not
expose port 8765 to the public internet. The bridge uses plain HTTP, so its
bearer token and traffic are not encrypted on the network.

Conversation metadata and Claude Code session IDs are stored in the local
`conversations.json` file. Do not share this file; it is ignored by Git.

## Endpoints

- `GET /health` reports whether the bridge is responding.
- `GET /usage` returns Claude Code usage information (authorized).
- `GET /conversations` lists saved conversations (authorized).
- `DELETE /conversations/:id` deletes one saved conversation (authorized).
- `POST /chat` sends a message (authorized).

All endpoints except `/health` require `Authorization: Bearer <token>`.

