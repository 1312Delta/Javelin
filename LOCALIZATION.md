# Localization Guide

This guide explains how to add or update translations for Javelin.

## Overview

Javelin uses a JSON-based localization system. Translation files are stored in `romfs/javelin/i18n/` with the naming convention `<locale>.json` (e.g., `en.json`, `ja.json`).

## Translation Keys

Translation keys are organized by feature:

| Prefix | Description |
|--------|-------------|
| `app.*` | Application metadata |
| `menu.*` | Main menu items |
| `mtp.*` | MTP screen strings |
| `tickets.*` | Ticket browser strings |
| `modal.*` | Modal dialogs |
| `settings.*` | Settings screen |
| `icon.*` | Notification icons |

## Adding a New Language

### Option 1: Via Crowdin (Recommended)

1. Visit the [Javelin Crowdin project](https://crowdin.com/project/javelin)
2. Request a new language if not already available
3. Translate using the web interface
4. Translations will be included in the next release

### Option 2: Manual Translation

1. **Copy the source file:**
   ```bash
   cp romfs/javelin/i18n/en.json romfs/javelin/i18n/<locale>.json
   ```

2. **Edit the new file, translating only the values:**
   ```json
   {
     "app.subtitle": "Nintendo Switch Toolkit",  // Keep keys unchanged
     "menu.mtp_title": "MTP File Transfer"       // Translate this value
   }
   ```

3. **Add the language to `source/i18n/Localization.cpp`:**
   ```cpp
   } else if (code == "ja") {
       languages.push_back({"ja", "Japanese", "日本語"});
   ```

4. **Rebuild the project:**
   ```bash
   make clean && make -j$(nproc)
   ```

## Language Codes

Use ISO 639-1 two-letter codes:

| Code | Language |
|------|----------|
| en | English |
| ja | Japanese |
| fr | French |
| de | German |
| es | Spanish |
| it | Italian |
| nl | Dutch |
| pt | Portuguese |
| ru | Russian |
| zh | Chinese |
| ko | Korean |

## Testing Translations

1. Build Javelin with the new translation file
2. Launch Javelin on Switch
3. Go to Settings
4. Select your language from the dropdown
5. Navigate through the app to verify translations

## Common Issues

### Text Too Long

Some UI elements have limited space. If a translation is too long:
- Consider using abbreviations
- Use shorter equivalent phrases
- Report the issue for UI adjustment

### Special Characters

JSON requires escaping certain characters:
- Quotes: `"` → `\"`
- Backslash: `\` → `\\`
- Newlines: Use `\n`

## Contributing

We welcome all translation contributions! Please:

1. Test your translations thoroughly
2. Ensure consistent terminology
3. Follow the existing style and tone
4. Submit via Crowdin or pull request
