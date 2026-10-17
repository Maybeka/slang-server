# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT

import asyncio
from pathlib import Path

import pytest
from lsprotocol import types

from .conftest import Flags, SlangClient
from .test_package_header import position


@pytest.mark.asyncio
async def test_header_only_workspace_context(my_options: Flags, tmp_path: Path) -> None:
    owner = tmp_path / "owner.sv"
    header = tmp_path / "item.svh"
    owner.write_text(
        'package p; typedef int value_t;\n`include "item.svh"\nendpackage\n'
    )
    original = "class item;\nvalue_t value;\nendclass\n"
    header.write_text(original)
    client = SlangClient(workspace_root=str(tmp_path))
    await client.start(my_options)
    try:
        await client.initialize_async(
            types.InitializeParams(
                capabilities=types.ClientCapabilities(
                    text_document=types.TextDocumentClientCapabilities(
                        definition=types.DefinitionClientCapabilities(link_support=True)
                    )
                ),
                root_uri=tmp_path.as_uri(),
                workspace_folders=[
                    types.WorkspaceFolder(uri=tmp_path.as_uri(), name="test")
                ],
            )
        )
        client.initialized(types.InitializedParams())
        pending = client.protocol.wait_for_notification(
            types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS
        )
        client.openText(header.as_uri(), original)
        diagnostics = await asyncio.wait_for(asyncio.wrap_future(pending), 10)
        assert diagnostics.uri == header.as_uri()
        assert diagnostics.diagnostics == []

        async def definition(text: str):
            result = await asyncio.wait_for(
                client.text_document_definition_async(
                    types.DefinitionParams(
                        text_document=types.TextDocumentIdentifier(header.as_uri()),
                        position=position(text, "value_t value"),
                    )
                ),
                10,
            )
            assert result and len(result) == 1
            assert result[0].target_uri == owner.as_uri()

        await definition(original)
        edited = "\n\n" + original
        pending = client.protocol.wait_for_notification(
            types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS
        )
        client.text_document_did_change(
            types.DidChangeTextDocumentParams(
                text_document=types.VersionedTextDocumentIdentifier(
                    uri=header.as_uri(), version=1
                ),
                content_changes=[
                    types.TextDocumentContentChangeEvent_Type2(text=edited)
                ],
            )
        )
        diagnostics = await asyncio.wait_for(asyncio.wrap_future(pending), 10)
        assert diagnostics.uri == header.as_uri()
        assert diagnostics.diagnostics == []
        await definition(edited)
        assert header.read_text() == original
    finally:
        await client.shutdown_async(None)
        client.exit(None)
        await client.stop()
