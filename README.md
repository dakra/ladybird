# Ladybird

[Ladybird](https://ladybird.org) is a truly independent web browser, using a novel engine based on web standards.

## About this fork

The `emacs-headless` branch adds a headless mode that serves pages to Emacs. It is the server for
[ladybird.el](https://github.com/dakra/ladybird.el), an Emacs package that shows Ladybird pages in a buffer.

`Ladybird --headless=emacs` opens no window. It renders pages, sends the changed pixels and page events to Emacs, and
takes keyboard, mouse and commands back, over standard input and output or a Unix socket. With a socket, pages outlive
the Emacs that shows them, and the server can run on another machine, reached over ssh. The protocol is described in
[plans/01-protocol.md](https://github.com/dakra/ladybird.el/blob/main/plans/01-protocol.md) of ladybird.el.

```bash
git clone --branch emacs-headless https://github.com/dakra/ladybird
cd ladybird
./Meta/ladybird.py build
```

See the [build instructions](Documentation/BuildInstructionsLadybird.md) for the dependencies, and the
[README of ladybird.el](https://github.com/dakra/ladybird.el#readme) for using it from Emacs.

This fork is not affiliated with the Ladybird project. The branch is rebased onto upstream from time to time. Please
report problems with the Emacs mode [to ladybird.el](https://github.com/dakra/ladybird.el/issues), not upstream.

The rest of this README is upstream's.

> [!IMPORTANT]
> Ladybird is in a pre-alpha state, and only suitable for use by developers
>

## Features

We aim to build a complete, usable browser for the modern web.

Ladybird uses a [multi-process architecture](Documentation/ProcessArchitecture.md) with a main UI process,
WebContent renderer processes, and helper processes for tasks such as networking, image decoding, compositing and media.

Documents from different sites run in separate renderer processes, each sandboxed from the rest of the system.
Image decoding and network connections are done out of process, separately for each site, to be more robust against
malicious content.

At the moment, many core library support components are inherited from SerenityOS:

- LibWeb: Web rendering engine
- LibJS: JavaScript engine
- LibWasm: WebAssembly implementation
- LibCrypto/LibTLS: Cryptography primitives and Transport Layer Security
- LibHTTP: HTTP/1.1 client
- LibGfx: 2D Graphics Library, Image Decoding and Rendering
- LibUnicode: Unicode and locale support
- LibMedia: Audio and video playback
- LibCore: Event loop, OS abstraction layer
- LibIPC: Inter-process communication

## How do I build and run this?

See [build instructions](Documentation/BuildInstructionsLadybird.md) for information on how to build Ladybird.

Ladybird runs on Linux, macOS, Windows (with WSL2), and many other \*Nixes.

## How do I read the documentation?

Code-related documentation can be found in the [documentation](Documentation/) folder.

## Get in touch and participate!

Join [our Discord server](https://discord.gg/nvfjVJ4Svh) to participate in issue and development discussions.

Please read [Getting involved with Ladybird](Documentation/GettingStartedContributing.md) if you're new to Ladybird and want to help.

Before opening an issue, please see the [issue policy](CONTRIBUTING.md#issue-policy) and the [detailed issue-reporting guidelines](ISSUES.md).

The project participation guidelines can be found in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

Ladybird is licensed under a 2-clause BSD license.
