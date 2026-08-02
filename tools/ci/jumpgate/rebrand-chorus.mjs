#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const webRoot = path.join(root, "addons", "webinterface.default");
const localeRoot = path.join(webRoot, "lang", "_strings");
const bundlePath = path.join(webRoot, "js", "kodi-webinterface.js");

function replaceAll(source, before, after) {
  return source.split(before).join(after);
}

function rebrandDisplayValue(value) {
  if (typeof value === "string") {
    const protectedPhrases = new Map([
      ["Powered by Kodi", "__JUMPGATE_POWERED_BY_KODI__"],
      ["Kodi.tv", "__JUMPGATE_KODI_DOT_TV__"],
    ]);
    let branded = value;
    for (const [phrase, marker] of protectedPhrases) {
      branded = branded.replaceAll(phrase, marker);
    }
    branded = branded.replaceAll("Kodi", "Jumpgate");
    for (const [phrase, marker] of protectedPhrases) {
      branded = branded.replaceAll(marker, phrase);
    }
    return branded;
  }
  if (Array.isArray(value)) return value.map(rebrandDisplayValue);
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value).map(([key, nested]) => [key, rebrandDisplayValue(nested)]),
    );
  }
  return value;
}

const localeFiles = fs
  .readdirSync(localeRoot)
  .filter((name) => name.endsWith(".json"))
  .sort();
const brandedKeys = new Set();

for (const name of localeFiles) {
  const file = path.join(localeRoot, name);
  const source = fs.readFileSync(file, "utf8");
  const trailingNewline = source.endsWith("\n");
  const document = JSON.parse(source);
  const messages = document?.locale_data?.messages;
  if (!messages || typeof messages !== "object") {
    throw new Error(`Missing gettext message map: ${file}`);
  }

  const renamed = {};
  for (const [key, value] of Object.entries(messages)) {
    const nextKey = key.replaceAll("Kodi", "Jumpgate");
    if (nextKey !== key) brandedKeys.add(key);
    if (Object.hasOwn(renamed, nextKey)) {
      throw new Error(`Chorus translation key collision in ${file}: ${nextKey}`);
    }
    renamed[nextKey] = rebrandDisplayValue(value);
  }
  document.locale_data.messages = renamed;
  fs.writeFileSync(file, JSON.stringify(document) + (trailingNewline ? "\n" : ""));
}

let bundle = fs.readFileSync(bundlePath, "utf8");
const protectedIdentifiers = [
  "this.Kodi",
  "Kodi.request",
  "Kodi.execute",
  "Kodi.navigate",
  "KodiEntities",
  "baseKodiUrl",
];
const protectedCounts = new Map(
  protectedIdentifiers.map((needle) => [needle, bundle.split(needle).length - 1])
);

for (const key of [...brandedKeys].sort((left, right) => right.length - left.length)) {
  const branded = key.replaceAll("Kodi", "Jumpgate");
  bundle = replaceAll(bundle, JSON.stringify(key), JSON.stringify(branded));
  if (!key.includes("'") && !branded.includes("'")) {
    bundle = replaceAll(bundle, `'${key}'`, `'${branded}'`);
  }
}

bundle = replaceAll(bundle, "appTitle: 'Kodi'", "appTitle: 'Jumpgate'");
bundle = replaceAll(bundle, '<strong>Kodi ', '<strong>Jumpgate ');
bundle = replaceAll(bundle, "A web interface for Kodi.", "A web interface for Jumpgate.");

for (const [needle, expected] of protectedCounts) {
  const actual = bundle.split(needle).length - 1;
  if (actual !== expected) {
    throw new Error(`Chorus runtime identifier changed: ${needle} (${expected} -> ${actual})`);
  }
}
fs.writeFileSync(bundlePath, bundle);

for (const name of fs.readdirSync(webRoot)) {
  if (!name.endsWith(".html")) continue;
  const file = path.join(webRoot, name);
  const source = fs.readFileSync(file, "utf8");
  fs.writeFileSync(file, source.replaceAll("Kodi", "Jumpgate"));
}

console.log(`Rebranded ${brandedKeys.size} Chorus message keys across ${localeFiles.length} locales.`);
