# Security policy

## Reporting a vulnerability

Please do **not** open a public issue for security-sensitive reports. Contact the maintainers
privately with:

- affected version/commit and platform;
- a minimal reproduction or description;
- impact assessment (crash, memory corruption, privilege boundary, etc.);
- whether a fix can be coordinated before disclosure.

If a private channel is not yet configured, open a minimal issue asking for a security contact
without including exploit details.

## Scope

This project loads media streams and interacts with GPU drivers, DMA-BUF, and external
libraries. Treat malformed H.264 streams, untrusted FFmpeg/Mesa inputs, and GPU driver
resources as untrusted. The compatibility layer is not a sandbox and does not add memory
safety guarantees to third-party decoders.
