# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT

import asyncio
from pathlib import Path

import pytest
from lsprotocol import types

from .conftest import Flags, LanguageClientProtocol, SlangClient
from .test_package_header import position


class DiagnosticProtocol(LanguageClientProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.diagnostic_events = asyncio.Queue()

    def _handle_notification(self, method_name, params):
        if method_name == types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS:
            self.diagnostic_events.put_nowait(params)
        super()._handle_notification(method_name, params)


@pytest.mark.asyncio
async def test_incremental_sibling_burst_preserves_requests_and_versions(
    my_options: Flags, tmp_path: Path
) -> None:
    (tmp_path / "owner.sv").write_text(
        "package p; typedef int value_t;\n"
        '`include "first.svh"\n`include "second.svh"\nendpackage\n'
    )
    paths = [tmp_path / "first.svh", tmp_path / "second.svh"]
    originals = [
        f"class {name};\nvalue_t value;\n"
        "function value_t get(); return value; endfunction\nendclass\n"
        for name in ("first", "second")
    ]
    for path, text in zip(paths, originals):
        path.write_text(text)
    client = SlangClient(protocol_cls=DiagnosticProtocol, workspace_root=str(tmp_path))
    await client.start(my_options)
    try:
        await client.initialize_async(
            types.InitializeParams(
                capabilities=types.ClientCapabilities(
                    text_document=types.TextDocumentClientCapabilities(
                        definition=types.DefinitionClientCapabilities(
                            link_support=True
                        ),
                        publish_diagnostics=types.PublishDiagnosticsClientCapabilities(
                            version_support=True
                        ),
                    )
                ),
                root_uri=tmp_path.as_uri(),
            )
        )
        client.initialized(types.InitializedParams())

        async def diagnostic(uri, version):
            while True:
                result = await asyncio.wait_for(
                    client.protocol.diagnostic_events.get(), 10
                )
                if result.uri == uri and result.version == version:
                    return result.diagnostics

        for path, text in zip(paths, originals):
            client.openText(path.as_uri(), text)
            assert await diagnostic(path.as_uri(), 0) == []

        texts = originals.copy()
        queries = []
        # Every delta depends on all preceding deltas. Requests between edits must
        # observe their FIFO snapshot even when later edits cancel diagnostic work.
        for version in range(1, 9):
            for index, path in enumerate(paths):
                texts[index] = "\n" + texts[index]
                client.text_document_did_change(
                    types.DidChangeTextDocumentParams(
                        text_document=types.VersionedTextDocumentIdentifier(
                            uri=path.as_uri(), version=version
                        ),
                        content_changes=[
                            types.TextDocumentContentChangeEvent_Type1(
                                range=types.Range(
                                    start=types.Position(0, 0), end=types.Position(0, 0)
                                ),
                                text="\n",
                            )
                        ],
                    )
                )
                if version % 2 == 0:
                    future = asyncio.ensure_future(
                        client.text_document_definition_async(
                            types.DefinitionParams(
                                text_document=types.TextDocumentIdentifier(
                                    path.as_uri()
                                ),
                                position=position(texts[index], "value; endfunction"),
                            )
                        )
                    )
                    queries.append((future, path.as_uri(), version + 1))
                    await asyncio.sleep(0)
        for future, uri, line in queries:
            result = await asyncio.wait_for(future, 10)
            assert result and len(result) == 1
            assert result[0].target_uri == uri
            assert result[0].target_selection_range.start.line == line

        uri = paths[0].as_uri()
        broken = texts[0].replace("value_t value", "latest_missing_t value")
        for version, text in ((9, broken), (10, texts[0])):
            client.text_document_did_change(
                types.DidChangeTextDocumentParams(
                    text_document=types.VersionedTextDocumentIdentifier(
                        uri=uri, version=version
                    ),
                    content_changes=[
                        types.TextDocumentContentChangeEvent_Type2(text=text)
                    ],
                )
            )
            diagnostics = await diagnostic(uri, version)
            if version == 9:
                assert any("latest_missing_t" in item.message for item in diagnostics)
            else:
                assert diagnostics == []
        for path, original in zip(paths, originals):
            assert path.read_text() == original
    finally:
        await client.shutdown_async(None)
        client.exit(None)
        await client.stop()
