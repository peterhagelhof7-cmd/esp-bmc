#pragma once

// Statische Login-Seite als C-String eingebettet, NICHT ueber
// EMBED_TXTFILES/target_add_binary_data() - beide erzeugten unter
// PlatformIOs espidf-Integration einen kaputten doppelten Pfad im
// generierten .S-Dateinamen ("Source ... not found" beim Bauen),
// unabhaengig davon, aus welcher CMakeLists.txt der Aufruf kam (main oder
// die eigene Komponente). Bei dieser Groesse (~1,4 KB) ist ein einfacher
// C-String die pragmatischere Loesung als das kaputte Embed-Tooling weiter
// zu verfolgen - siehe docs/entscheidungen.md.
static const char LOGIN_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"de\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>ESP-BMC - Anmeldung</title>\n"
    "<style>\n"
    "  body { font-family: sans-serif; background: #f2f0e9; display: flex; align-items: center; justify-content: "
    "center; height: 100vh; margin: 0; }\n"
    "  form { background: #fff; padding: 2rem; border-radius: 6px; border: 1px solid #e4e1d8; min-width: 260px; }\n"
    "  h1 { font-size: 1.1rem; margin: 0 0 1rem 0; color: #1c2430; }\n"
    "  label { display: block; font-size: 0.85rem; margin-bottom: 0.25rem; color: #444b57; }\n"
    "  input { width: 100%; padding: 0.5rem; margin-bottom: 1rem; box-sizing: border-box; border: 1px solid #ccc; "
    "border-radius: 4px; }\n"
    "  button { width: 100%; padding: 0.6rem; background: #0f1f3d; color: #fff; border: none; border-radius: 4px; "
    "cursor: pointer; }\n"
    "  .error { color: #a63d2e; font-size: 0.85rem; margin-bottom: 1rem; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<form method=\"post\" action=\"/login\">\n"
    "  <h1>ESP-BMC Anmeldung</h1>\n"
    "  <div class=\"error\" id=\"error\"></div>\n"
    "  <label for=\"u\">Benutzername</label>\n"
    "  <input type=\"text\" id=\"u\" name=\"username\" autofocus required>\n"
    "  <label for=\"p\">Passwort</label>\n"
    "  <input type=\"password\" id=\"p\" name=\"password\" required>\n"
    "  <button type=\"submit\">Anmelden</button>\n"
    "</form>\n"
    "<script>\n"
    "  if (location.search.includes(\"failed=1\")) {\n"
    "    document.getElementById(\"error\").textContent = \"Benutzername oder Passwort falsch.\";\n"
    "  }\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";
