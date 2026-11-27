# SPDX-License-Identifier: MIT
"""Keep the distributed configuration schema aligned with server defaults."""

import json
import subprocess
from pathlib import Path


def test_config_schema_defaults(my_options):
    binary = Path(my_options.binary).resolve()
    generator = binary.with_name("gen_config_schema" + binary.suffix)
    generated = json.loads(subprocess.check_output([str(generator)]))
    defaults = json.loads(subprocess.check_output([str(generator), "--defaults"]))
    repo = Path(__file__).resolve().parents[2]
    shipped = json.loads((repo / "clients/vscode/resources/config.schema.json").read_text())
    assert generated == shipped

    definitions = generated["$defs"]

    def check(schema, values):
        for name, property_schema in schema["properties"].items():
            assert "default" in property_schema, name
            assert property_schema["default"] == values.get(name), name
            if "$ref" in property_schema and isinstance(values.get(name), dict):
                target = property_schema["$ref"].removeprefix("#/$defs/")
                check(definitions[target], values[name])

    check(definitions["Config"], defaults)
