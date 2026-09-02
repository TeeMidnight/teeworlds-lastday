import json
import os
import re
import time
import sys

from collections import defaultdict

os.chdir(os.path.dirname(os.path.realpath(sys.argv[0])) + "/..")

format = "{0:40} {1:8} {2:8} {3:8}".format
SOURCE_EXTS = [".c", ".cpp", ".h"]

JSON_KEY_AUTHORS="authors"
JSON_KEY_CLIENT="client strings"
JSON_KEY_SERVER="server strings"
JSON_KEY_ITEM="item strings"
JSON_KEY_CTXT="context"
JSON_KEY_FROM="from"
JSON_KEY_OR="or"
JSON_KEY_TR="tr"

SOURCE_LOCALIZE_RE=re.compile(br'Localize\("(?P<str>([^"\\]|\\.)*)"(, ?"(?P<ctxt>([^"\\]|\\.)*)")?\)')

# contexts used for item names and descriptions in the inventory menu
ITEM_NAME_CTXT = "Item Name"
ITEM_DESC_CTXT = "Item Desc"
# context used for item types shown in the crafting menu
ITEM_TYPE_CTXT = "Item Type"

def item_types_of(item):
	"""Normalizes the item_type field (a single string or a list) to a list of types."""
	types = item.get("item_type")
	if isinstance(types, str):
		return [types] if types else []
	if isinstance(types, list):
		return [t for t in types if isinstance(t, str) and t]
	return []

def parse_item_names():
	"""Collects the item names and descriptions from datasrc/items/*.json so
	they can be translated in the inventory menu."""
	l10n = defaultdict(lambda: str)

	items_dir = "datasrc/items"
	if not os.path.isdir(items_dir):
		return l10n

	for filename in sorted(os.listdir(items_dir)):
		if not filename.endswith(".json"):
			continue
		with open(os.path.join(items_dir, filename), encoding='utf-8') as f:
			item = json.load(f)
		if not isinstance(item, dict):
			continue
		if item.get("name"):
			l10n[(item["name"], ITEM_NAME_CTXT)] = ""
		desc = item.get("desc") or item.get("Desc")
		if desc:
			# the description context carries the item name so that two items
			# sharing the same description text get separate translations
			l10n[(desc, ITEM_DESC_CTXT + ": " + item["name"])] = ""
		for type_id in item_types_of(item):
			l10n[(type_id, ITEM_TYPE_CTXT)] = ""
	return l10n

def parse_source():
	l10n_client = defaultdict(lambda: str)
	l10n_server = defaultdict(lambda: str)

	def process_line(line, l10n):
		for match in SOURCE_LOCALIZE_RE.finditer(line):
			str_ = match.group('str').decode()
			ctxt = match.group('ctxt')
			if ctxt is not None:
				ctxt = ctxt.decode()
			l10n[(str_, ctxt)] = ""

	for root, dirs, files in os.walk("src"):
		for name in files:
			filename = os.path.join(root, name)
			
			if os.sep + "external" + os.sep in filename:
				continue

			if os.path.splitext(filename)[1] in SOURCE_EXTS:
				# HACK: Open source as binary file.
				# Necessary some of teeworlds source files
				# aren't utf-8 yet for some reason
				for line in open(filename, 'rb'):
					# process line
					if "client" in filename:
						process_line(line, l10n_client)
					else:
						process_line(line, l10n_server)
	return l10n_client, l10n_server

def load_languagefile(filename):
	return json.load(open(filename), strict=False) # accept \t tabs

def write_languagefile(outputfilename, l10n_client_src, l10n_server_src, l10n_item_src, old_l10n_data):
	def merge_old(src_key, l10n_src):
		"""Keep an existing translation only if its key is still present in the
		newly extracted source for that section."""
		translations = l10n_src.copy()
		translations.update({
			(t[JSON_KEY_OR], t.get(JSON_KEY_CTXT)): t[JSON_KEY_TR]
			for t in old_l10n_data.get(src_key, [])
			if t[JSON_KEY_TR] and translations.get((t[JSON_KEY_OR], t.get(JSON_KEY_CTXT))) != None
		})
		return translations

	def build_array(translations):
		array = []
		for entry in translations:
			if entry[0]:
				t_entry = {}
				t_entry[JSON_KEY_OR] = entry[0]
				t_entry[JSON_KEY_TR] = translations[entry]
				if entry[1] is not None:
					t_entry[JSON_KEY_CTXT] = entry[1]
				array.append(t_entry)
		return array

	result = {
		JSON_KEY_CLIENT: build_array(merge_old(JSON_KEY_CLIENT, l10n_client_src)),
		JSON_KEY_SERVER: build_array(merge_old(JSON_KEY_SERVER, l10n_server_src)),
		JSON_KEY_ITEM: build_array(merge_old(JSON_KEY_ITEM, l10n_item_src)),
		JSON_KEY_AUTHORS: old_l10n_data.get(JSON_KEY_AUTHORS),
	}

	for key in (JSON_KEY_CLIENT, JSON_KEY_SERVER, JSON_KEY_ITEM):
		result[key].sort(key=lambda entry: entry[JSON_KEY_OR])

	json.dump(
		result,
		open(outputfilename, 'w', encoding='utf-8'),
		ensure_ascii=False,
		indent="\t",
		separators=(',', ': '),
		sort_keys=True,
	)

if __name__ == '__main__':
	l10n_client, l10n_server = parse_source()

	# item names, descriptions and types live in their own "item strings" section
	l10n_item = parse_item_names()

	for filename in os.listdir("datasrc/languages"):
		try:
			if (os.path.splitext(filename)[1] == ".json"
					and filename != "index.json"):
				filename = "datasrc/languages/" + filename
				write_languagefile(filename, l10n_client, l10n_server, l10n_item, load_languagefile(filename))
		except Exception as e:
			print("Failed on {0}, re-raising for traceback".format(filename))
			raise
