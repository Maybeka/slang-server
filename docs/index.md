---
hide:
  # - toc
  - navigation
  - feedback
---

`slang-server` is a high-performance [Language Server Protocol (LSP)](https://microsoft.github.io/language-server-protocol/) implementation for SystemVerilog, built on top of the [Slang](https://github.com/MikePopoloski/slang) SystemVerilog frontend.

## Overview

slang-server provides comprehensive SystemVerilog language support for modern editors and IDEs, including:

- **Real-time diagnostics and linting**
- **Code completions (IntelliSense)**
- **Go-to definition and Go-to references**
- **Symbol search and workspace indexing**
- **Hardware-specific language features via custom editor clients**

## Quick Start

Check out the [installation guide](start/installing.md) and learn how to [configure your editor](start/config.md).

## Project Status

This project maintains its language server independently and builds on official slang sources with a project-owned compiler patch. The [maintenance policy](development/versioning.md) explains how language features and performance changes are selected.

## Support & Contributing

- **Bug Reports**: Open an issue on [GitHub](https://github.com/Maybeka/slang-server)
- **Contributing**: Pull requests are welcome! See our [contributing guidelines](https://github.com/Maybeka/slang-server/blob/main/CONTRIBUTING.md).

## License

slang-server is licensed under the MIT License. See the [full license](https://github.com/Maybeka/slang-server/blob/main/LICENSE) for details.
