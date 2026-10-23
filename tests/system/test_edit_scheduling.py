# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT

import asyncio
import json
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
@pytest.mark.parametrize("frozen_sources", [False, True])
async def test_incremental_sibling_burst_preserves_requests_and_versions(
    my_options: Flags, tmp_path: Path, frozen_sources: bool
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
    if frozen_sources:
        (tmp_path / "design.f").write_text("owner.sv\n")
        (tmp_path / ".slang").mkdir()
        (tmp_path / ".slang/server.json").write_text(
            json.dumps(
                {
                    "compilationFilelist": "design.f",
                    "experimentalFrozenSources": True,
                    "frozenSources": {"packages": ["p"]},
                }
            )
        )
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


@pytest.mark.asyncio
async def test_scope_reuse_publishes_complete_natural_diagnostics(
    my_options: Flags, tmp_path: Path
) -> None:
    """An idle edit must update the whole design without a query or didSave."""
    provider = tmp_path / "fixed.sv"
    consumer = tmp_path / "consumer.sv"
    provider.write_text(
        "module stable(output wire [31:0] y);\n"
        "logic [31:0] q, spare; assign y=q;\n"
        "string name; int fd;\n"
        'initial begin $sformat(name,"%s","unused-simulation-file"); '
        'fd=$fopen(name,"w"); $fwrite(fd,"%s",name); $fclose(fd); end\n'
        "endmodule\n"
    )

    def source(read: bool, broken: bool) -> str:
        return (
            "module top; wire [31:0] y; stable u(.*);\n"
            + ("initial $display(u.spare);\n" if read else "")
            + ("initial latest_missing_name=1;\n" if broken else "")
            + "endmodule\n"
        )

    original = source(False, False)
    consumer.write_text(original)
    (tmp_path / "design.f").write_text("fixed.sv\nconsumer.sv\n")
    (tmp_path / ".slang").mkdir()
    uris = {provider.as_uri(), consumer.as_uri()}
    results = []
    for enabled in (False, True):
        (tmp_path / ".slang/server.json").write_text(
            json.dumps(
                {
                    "compilationFilelist": "design.f",
                    "flags": "--single-unit --threads 1 --top top",
                    "experimentalScopeSemanticCache": enabled,
                    "frozenSources": {"instances": ["top.u"], "maxMiB": 64},
                }
            )
        )
        client = SlangClient(protocol_cls=DiagnosticProtocol, workspace_root=str(tmp_path))
        await client.start(my_options)
        try:
            await client.initialize_async(
                types.InitializeParams(
                    capabilities=types.ClientCapabilities(
                        text_document=types.TextDocumentClientCapabilities(
                            publish_diagnostics=types.PublishDiagnosticsClientCapabilities(
                                version_support=True
                            )
                        )
                    ),
                    root_uri=tmp_path.as_uri(),
                )
            )
            client.initialized(types.InitializedParams())
            await client.workspace_execute_command_async(
                types.ExecuteCommandParams(
                    command="slang.setBuildFile", arguments=[str(tmp_path / "design.f")]
                )
            )
            client.openText(consumer.as_uri(), original)
            while True:
                event = await asyncio.wait_for(client.protocol.diagnostic_events.get(), 10)
                if event.uri == consumer.as_uri() and event.version == 0:
                    break
            # A foreground response drains initialization; no RPC follows an edit
            # until every source's natural diagnostic publication is received.
            await client.workspace_execute_command_async(
                types.ExecuteCommandParams(command="slang.scopeLibraryStatus", arguments=[])
            )
            while not client.protocol.diagnostic_events.empty():
                client.protocol.diagnostic_events.get_nowait()
            async def finish_committed_batch(batch):
                # Edited URI publishes first; a closed provider has no document version.
                # One event per URI can therefore combine two committed generations.
                # This cached maintenance query never requests analysis. Its response
                # fences the remainder of an already observed natural stdio publication.
                await client.workspace_execute_command_async(
                    types.ExecuteCommandParams(command="slang.scopeLibraryStatus", arguments=[])
                )
                while not client.protocol.diagnostic_events.empty():
                    event = client.protocol.diagnostic_events.get_nowait()
                    assert event.uri in uris
                    batch[event.uri] = event
                assert batch[consumer.as_uri()].version == version

            states = []
            for version, (read, broken) in enumerate(
                ((True, False), (False, True), (False, False)) * 2, 1
            ):
                client.text_document_did_change(
                    types.DidChangeTextDocumentParams(
                        text_document=types.VersionedTextDocumentIdentifier(
                            uri=consumer.as_uri(), version=version
                        ),
                        content_changes=[
                            types.TextDocumentContentChangeEvent_Type2(text=source(read, broken))
                        ],
                    )
                )
                batch = {}
                while batch.keys() != uris or batch[consumer.as_uri()].version != version:
                    event = await asyncio.wait_for(client.protocol.diagnostic_events.get(), 10)
                    assert event.uri in uris
                    batch[event.uri] = event
                await finish_committed_batch(batch)
                diagnostics = batch[consumer.as_uri()].diagnostics
                assert any("latest_missing_name" in d.message for d in diagnostics) == broken
                assert any(
                    d.code == "unused-variable" and "spare" in d.message
                    for d in batch[provider.as_uri()].diagnostics
                ) == (not read)
                states.append(batch)
            # Real range edits must retire superseded analyses while preserving
            # the current provider warnings, consumer errors and repair batch.
            version = 6
            for read, broken in ((True, False), (False, True), (False, False)):
                text = source(read, broken) + "// typing: \n"
                line = text.count("\n") - 1
                version += 1
                client.text_document_did_change(
                    types.DidChangeTextDocumentParams(
                        text_document=types.VersionedTextDocumentIdentifier(
                            uri=consumer.as_uri(), version=version
                        ),
                        content_changes=[types.TextDocumentContentChangeEvent_Type2(text=text)],
                    )
                )
                for character in range(32):
                    version += 1
                    at = types.Position(line, len("// typing: ") + character)
                    client.text_document_did_change(
                        types.DidChangeTextDocumentParams(
                            text_document=types.VersionedTextDocumentIdentifier(
                                uri=consumer.as_uri(), version=version
                            ),
                            content_changes=[
                                types.TextDocumentContentChangeEvent_Type1(
                                    range=types.Range(start=at, end=at), text="x"
                                )
                            ],
                        )
                    )
                    if character % 8 == 0:
                        await asyncio.sleep(0)
                batch = {}
                last_published = states[-1][consumer.as_uri()].version
                while batch.keys() != uris or batch[consumer.as_uri()].version != version:
                    event = await asyncio.wait_for(client.protocol.diagnostic_events.get(), 10)
                    assert event.uri in uris
                    if event.uri == consumer.as_uri():
                        assert last_published <= event.version <= version
                        last_published = event.version
                    batch[event.uri] = event
                await finish_committed_batch(batch)
                assert any(
                    "latest_missing_name" in d.message
                    for d in batch[consumer.as_uri()].diagnostics
                ) == broken
                assert any(
                    d.code == "unused-variable" and "spare" in d.message
                    for d in batch[provider.as_uri()].diagnostics
                ) == (not read)
                states.append(batch)
            results.append(states)
            status = await client.workspace_execute_command_async(
                types.ExecuteCommandParams(command="slang.scopeLibraryStatus", arguments=[])
            )
            if enabled:
                assert status["prepared"] and status["bodies"]["hits"] > 0
        finally:
            await client.shutdown_async(None)
            client.exit(None)
            await client.stop()
    assert results[0] == results[1]
    assert consumer.read_text() == original
    assert not (tmp_path / "unused-simulation-file").exists()
