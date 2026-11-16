"""Tests for the in-app agent's settings (pbr_agent.settings): where each
setting comes from, and what a broken settings file does."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pbr_agent.settings import SettingsError, resolve  # noqa: E402

NOTHING = {"provider": None, "model": None, "effort": None}


class SettingsTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "agent.json"

    def tearDown(self):
        self.dir.cleanup()

    def write(self, values):
        self.path.write_text(values if isinstance(values, str) else json.dumps(values), encoding="utf-8")

    def resolve(self, command_line=None, environ=None):
        return resolve(command_line or NOTHING, environ=environ or {}, path=self.path)

    def test_defaults_without_a_file(self):
        settings = self.resolve()
        self.assertEqual(settings["provider"].value, "claude")
        self.assertEqual(settings["provider"].source, "default")
        self.assertIsNone(settings["model"].value)
        self.assertEqual(settings["effort"].value, "medium")

    def test_the_file_is_used_when_nothing_else_says(self):
        self.write({"provider": "gemini", "model": "gemini-3.8-pro"})
        settings = self.resolve()
        self.assertEqual(settings["provider"].value, "gemini")
        self.assertEqual(settings["provider"].source, str(self.path))
        self.assertEqual(settings["model"].value, "gemini-3.8-pro")
        self.assertEqual(settings["effort"].source, "default")

    def test_the_environment_beats_the_file_and_the_command_line_beats_both(self):
        self.write({"provider": "gemini", "model": "from-file"})
        settings = self.resolve(environ={"PBR_AGENT_PROVIDER": "ollama"})
        self.assertEqual(settings["provider"].value, "ollama")
        self.assertEqual(settings["provider"].source, "$PBR_AGENT_PROVIDER")
        self.assertEqual(settings["model"].value, "from-file")

        settings = self.resolve({**NOTHING, "provider": "claude"}, environ={"PBR_AGENT_PROVIDER": "ollama"})
        self.assertEqual(settings["provider"].value, "claude")
        self.assertEqual(settings["provider"].source, "command line")

    def test_an_empty_variable_counts_as_unset(self):
        self.write({"provider": "gemini"})
        self.assertEqual(self.resolve(environ={"PBR_AGENT_PROVIDER": ""})["provider"].value, "gemini")

    def test_a_broken_file_says_what_is_wrong(self):
        cases = {
            "{not json": "isn't valid JSON",
            "[1, 2]": "JSON object",
            json.dumps({"provider": "gpt"}): "must be one of",
            json.dumps({"effort": "huge"}): "must be one of",
            json.dumps({"api_key": "x"}): "unknown setting",
            json.dumps({"model": 3}): "should be a string",
        }
        for text, message in cases.items():
            with self.subTest(text=text):
                self.write(text)
                with self.assertRaises(SettingsError) as raised:
                    self.resolve()
                self.assertIn(message, str(raised.exception))

    def test_an_unknown_provider_from_the_environment_is_an_error_too(self):
        with self.assertRaises(SettingsError) as raised:
            self.resolve(environ={"PBR_AGENT_PROVIDER": "Gemini"})
        self.assertIn("$PBR_AGENT_PROVIDER", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
