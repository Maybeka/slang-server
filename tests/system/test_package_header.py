# SPDX-FileCopyrightText: Hudson River Trading
# SPDX-License-Identifier: MIT

import asyncio
from pathlib import Path

import pytest
from lsprotocol import types

from .conftest import SlangClient


def position(text: str, needle: str) -> types.Position:
    offset = text.index(needle)
    return types.Position(
        line=text.count("\n", 0, offset),
        character=offset - text.rfind("\n", 0, offset) - 1,
    )


@pytest.mark.asyncio
async def test_package_header_unsaved_context(client: SlangClient) -> None:
    root = Path(__file__).resolve().parent.parent / "data" / "included_package"
    owner_path = root / "owner.sv"
    header_path = root / "item.svh"
    original = header_path.read_text()
    owner_uri = owner_path.as_uri()
    header_uri = header_path.as_uri()

    async def opened(uri: str, text: str) -> None:
        pending = client.protocol.wait_for_notification(
            types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS
        )
        client.openText(uri, text)
        diagnostics = await asyncio.wait_for(asyncio.wrap_future(pending), 10)
        assert diagnostics.uri == uri
        assert diagnostics.diagnostics == []

    async def changed(text: str, version: int):
        pending = client.protocol.wait_for_notification(
            types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS
        )
        client.text_document_did_change(
            types.DidChangeTextDocumentParams(
                text_document=types.VersionedTextDocumentIdentifier(
                    uri=header_uri, version=version
                ),
                content_changes=[types.TextDocumentContentChangeEvent_Type2(text=text)],
            )
        )
        diagnostics = await asyncio.wait_for(asyncio.wrap_future(pending), 10)
        assert diagnostics.uri == header_uri
        return diagnostics.diagnostics

    async def definition(text: str, needle: str):
        return await asyncio.wait_for(
            client.text_document_definition_async(
                types.DefinitionParams(
                    text_document=types.TextDocumentIdentifier(header_uri),
                    position=position(text, needle),
                )
            ),
            10,
        )

    await opened(owner_uri, owner_path.read_text())
    await opened(header_uri, original)
    definitions = await definition(original, "value_t value")
    assert definitions and len(definitions) == 1
    assert definitions[0].target_uri == owner_uri

    edited = "\n\n" + original
    assert await changed(edited, 1) == []
    definitions = await definition(edited, "value;\n    endfunction")
    assert definitions and len(definitions) == 1
    assert definitions[0].target_uri == header_uri
    assert definitions[0].target_selection_range.start.line == 4

    completion = await asyncio.wait_for(
        client.text_document_completion_async(
            types.CompletionParams(
                text_document=types.TextDocumentIdentifier(header_uri),
                position=position(edited, "value;\n    endfunction"),
            )
        ),
        10,
    )
    items = (
        completion.items if isinstance(completion, types.CompletionList) else completion
    )
    assert items and any(item.label == "value" for item in items)

    broken = edited.replace("value_t value", "missing_t value")
    diagnostics = await changed(broken, 2)
    assert any("missing_t" in diagnostic.message for diagnostic in diagnostics)
    assert await changed(original, 3) == []
    assert header_path.read_text() == original


@pytest.mark.asyncio
async def test_completion_without_optional_context(client: SlangClient) -> None:
    uri = "file:///completion_without_context.sv"
    text = (
        "class completion_case;\n"
        "    int value;\n"
        "    function int get();\n"
        "        return value;\n"
        "    endfunction\n"
        "endclass\n"
    )
    client.openText(uri, text)
    completion = await asyncio.wait_for(
        client.text_document_completion_async(
            types.CompletionParams(
                text_document=types.TextDocumentIdentifier(uri),
                position=position(text, "value;\n    endfunction"),
            )
        ),
        10,
    )
    items = (
        completion.items if isinstance(completion, types.CompletionList) else completion
    )
    assert items and any(item.label == "value" for item in items)
