using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace ChaosMod.Core.Config
{
    /// <summary>
    /// Minimal ini reader/writer that preserves section/key ordering and comment
    /// lines (lines starting with ';' or '#') on round-trip, since the in-game
    /// menu writes live edits back to the same file a human may also hand-edit.
    /// Deliberately not SHVDN's ScriptSettings: this type lives in Core, which
    /// must have zero reference to SHVDN/GTA assemblies.
    /// </summary>
    public sealed class IniFile
    {
        private sealed class Line
        {
            public string Raw;
            public bool IsComment;
            public bool IsBlank;
            public string Section;
            public string Key;
            public string Value;
        }

        private readonly List<Line> _lines = new List<Line>();
        private readonly Dictionary<string, Dictionary<string, Line>> _index =
            new Dictionary<string, Dictionary<string, Line>>(StringComparer.OrdinalIgnoreCase);

        private string _path;

        public static IniFile Load(string path)
        {
            var ini = new IniFile { _path = path };
            if (File.Exists(path))
            {
                foreach (var rawLine in File.ReadAllLines(path))
                    ini.ParseLine(rawLine);
            }
            return ini;
        }

        private void ParseLine(string rawLine)
        {
            var trimmed = rawLine.Trim();
            var line = new Line { Raw = rawLine };

            if (trimmed.Length == 0)
            {
                line.IsBlank = true;
            }
            else if (trimmed.StartsWith(";") || trimmed.StartsWith("#"))
            {
                line.IsComment = true;
            }
            else if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
            {
                _currentSection = trimmed.Substring(1, trimmed.Length - 2).Trim();
                line.Section = _currentSection;
            }
            else
            {
                var eq = trimmed.IndexOf('=');
                if (eq > 0)
                {
                    var key = trimmed.Substring(0, eq).Trim();
                    var value = trimmed.Substring(eq + 1).Trim();
                    line.Section = _currentSection;
                    line.Key = key;
                    line.Value = value;

                    if (!string.IsNullOrEmpty(_currentSection))
                    {
                        if (!_index.TryGetValue(_currentSection, out var sectionDict))
                        {
                            sectionDict = new Dictionary<string, Line>(StringComparer.OrdinalIgnoreCase);
                            _index[_currentSection] = sectionDict;
                        }
                        sectionDict[key] = line;
                    }
                }
                else
                {
                    line.IsComment = true; // unparseable line, keep as-is on save
                }
            }

            _lines.Add(line);
        }

        private string _currentSection = "";

        public string GetRaw(string section, string key)
        {
            if (_index.TryGetValue(section, out var sectionDict) && sectionDict.TryGetValue(key, out var line))
                return line.Value;
            return null;
        }

        public string GetString(string section, string key, string defaultValue)
            => GetRaw(section, key) ?? defaultValue;

        public int GetInt(string section, string key, int defaultValue)
        {
            var raw = GetRaw(section, key);
            return raw != null && int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ? value : defaultValue;
        }

        public long GetLong(string section, string key, long defaultValue)
        {
            var raw = GetRaw(section, key);
            return raw != null && long.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value) ? value : defaultValue;
        }

        public bool GetBool(string section, string key, bool defaultValue)
        {
            var raw = GetRaw(section, key);
            return raw != null && bool.TryParse(raw, out var value) ? value : defaultValue;
        }

        public void Set(string section, string key, string value)
        {
            if (_index.TryGetValue(section, out var sectionDict) && sectionDict.TryGetValue(key, out var line))
            {
                line.Value = value;
                return;
            }

            // New key: append to the end of its section, creating the section if needed.
            var sectionExists = _lines.Any(l => l.Section == section && l.Key == null && !l.IsComment && !l.IsBlank);
            if (!sectionExists)
            {
                if (_lines.Count > 0) _lines.Add(new Line { IsBlank = true });
                _lines.Add(new Line { Section = section, Raw = $"[{section}]" });
            }

            var newLine = new Line { Section = section, Key = key, Value = value };

            var lastIndexOfSection = -1;
            for (var i = 0; i < _lines.Count; i++)
            {
                if (string.Equals(_lines[i].Section, section, StringComparison.OrdinalIgnoreCase))
                    lastIndexOfSection = i;
            }
            _lines.Insert(lastIndexOfSection + 1, newLine);

            if (!_index.TryGetValue(section, out var dict))
            {
                dict = new Dictionary<string, Line>(StringComparer.OrdinalIgnoreCase);
                _index[section] = dict;
            }
            dict[key] = newLine;
        }

        public void Save()
        {
            Save(_path);
        }

        public void Save(string path)
        {
            var sb = new StringBuilder();
            foreach (var line in _lines)
            {
                if (line.IsBlank) { sb.AppendLine(); continue; }
                if (line.IsComment) { sb.AppendLine(line.Raw); continue; }
                if (line.Key == null && line.Section != null) { sb.AppendLine($"[{line.Section}]"); continue; }
                if (line.Key != null) { sb.AppendLine($"{line.Key}={line.Value}"); continue; }
                sb.AppendLine(line.Raw);
            }

            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);

            File.WriteAllText(path, sb.ToString());
        }
    }
}
