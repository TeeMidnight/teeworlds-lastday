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
JSON_KEY_NAME="name"
JSON_KEY_CLIENT="client strings"
JSON_KEY_SERVER="server strings"
JSON_KEY_ITEM="item strings"
JSON_KEY_CTXT="context"
JSON_KEY_FROM="from"
JSON_KEY_OR="or"
JSON_KEY_TR="tr"

SOURCE_LOCALIZE_RE=re.compile(br'Localize\("(?P<str>([^"\\]|\\.)*)"(, ?"(?P<ctxt>([^"\\]|\\.)*)")?\)')
# LocalizeFormat("context", "format") -- the context comes first
SOURCE_LOCALIZE_FORMAT_RE=re.compile(br'LocalizeFormat\("(?P<ctxt>([^"\\]|\\.)*)"\s*,\s*"(?P<str>([^"\\]|\\.)*)"')
# SendSystemChat(client, "context", "text") -- the helper localizes internally
SOURCE_SEND_SYSTEM_CHAT_RE=re.compile(br'SendSystemChat\([^,]+,\s*"(?P<ctxt>([^"\\]|\\.)*)"\s*,\s*"(?P<str>([^"\\]|\\.)*)"')

# contexts used for item names and descriptions in the inventory menu
ITEM_NAME_CTXT = "Item Name"
ITEM_DESC_CTXT = "Item Desc"
# context used for item types shown in the crafting menu
ITEM_TYPE_CTXT = "Item Type"
# contexts used for the folder-derived categories in the crafting and inventory menus
CRAFT_CATEGORY_CTXT = "Craft Category"
ITEM_CATEGORY_CTXT = "Item Category"
# context used for the display names of the euclidean floors in the travel menu
WORLD_NAME_CTXT = "World Name"
# context used for the periodic chat tips
TIP_CTXT = "Tip"
# Script chat: ctx.player.chat("text", "context"). The second argument is the
# localization context; keep the literals in double quotes so this stays simple
# to extract.
SOURCE_SCRIPT_CHAT_RE = re.compile(r'\.chat\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"')

def item_types_of(item):
	"""Normalizes the item_type field (a single string or a list) to a list of types."""
	types = item.get("item_type")
	if isinstance(types, str):
		return [types] if types else []
	if isinstance(types, list):
		return [t for t in types if isinstance(t, str) and t]
	return []

def parse_item_names():
	"""Collects the item names and descriptions from datasrc/items/**.json so
	they can be translated in the inventory menu."""
	l10n = defaultdict(lambda: str)

	items_dir = "datasrc/items"
	if not os.path.isdir(items_dir):
		return l10n

	# items live in category sub-folders, so walk the whole tree
	for root, dirs, files in os.walk(items_dir):
		dirs.sort()
		for filename in sorted(files):
			if not filename.endswith(".json"):
				continue
			with open(os.path.join(root, filename), encoding='utf-8') as f:
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

def parse_category_folders(directory, context):
	"""Collects the category folder names under a data directory so menus can
	translate each folder name (weapon, tool, survival, misc...)."""
	l10n = {}
	if not os.path.isdir(directory):
		return l10n

	for name in sorted(os.listdir(directory)):
		if os.path.isdir(os.path.join(directory, name)):
			l10n[(name, context)] = ""
	return l10n

def parse_world_names():
	"""Collects the display names of the euclidean floors so the travel menu
	can translate them. The floor name is the JSON file name and the kind is
	the explicit "kind" field (older files derive it from their generation
	block)."""
	l10n = {}
	worlds_dir = "datasrc/maps/worlds"
	if not os.path.isdir(worlds_dir):
		return l10n

	for filename in sorted(os.listdir(worlds_dir)):
		if not filename.endswith(".json"):
			continue
		with open(os.path.join(worlds_dir, filename), encoding='utf-8') as f:
			world = json.load(f)
		if not isinstance(world, dict):
			continue
		kind = world.get("kind")
		if kind is None:
			kind = "euclid" if isinstance(world.get("generation"), dict) else "noneuclid"
		if kind != "euclid":
			continue
		name = os.path.splitext(filename)[0]
		l10n[(name, WORLD_NAME_CTXT)] = ""
	return l10n

def parse_container_names():
	"""Collects the display names of the storage containers. They live in
	datasrc/containers.json and are looked up by name with the "Container Name"
	context, so the translator needs them next to the other server strings."""
	l10n = {}
	containers_file = "datasrc/containers.json"
	if not os.path.isfile(containers_file):
		return l10n

	with open(containers_file, encoding='utf-8') as f:
		containers = json.load(f)
	if not isinstance(containers, dict):
		return l10n
	for container in containers.get("containers", []):
		if isinstance(container, dict) and container.get("name"):
			l10n[(container["name"], "Container Name")] = ""
	return l10n

def parse_tips():
	"""Collects the periodic chat tips from datasrc/tips.json. The server
	reads the same file at runtime and looks each entry up with the "Tip"
	context."""
	l10n = {}
	tips_file = "datasrc/tips.json"
	if not os.path.isfile(tips_file):
		return l10n

	with open(tips_file, encoding='utf-8') as f:
		tips = json.load(f)
	if not isinstance(tips, dict):
		return l10n
	for tip in tips.get("tips", []):
		if isinstance(tip, str) and tip:
			l10n[(tip, TIP_CTXT)] = ""
	return l10n

def parse_mob_dialogue():
	"""Collects dialogue lines from datasrc/mobs/*.json and configured NPCs.
	The runtime localizes each line with the "Mob Dialogue" context; they are
	data, not source literals, so the extractor has to read the same files."""
	l10n = {}
	paths = []
	for directory in ("datasrc/mobs", "datasrc/bots/npc"):
		if not os.path.isdir(directory):
			continue
		for filename in sorted(os.listdir(directory)):
			if filename.endswith(".json"):
				paths.append(os.path.join(directory, filename))

	for path in paths:
		try:
			with open(path, encoding='utf-8') as f:
				data = json.load(f)
		except (OSError, ValueError):
			continue
		if not isinstance(data, dict):
			continue
		dialogue = data.get("dialogue")
		if not isinstance(dialogue, dict):
			continue
		for value in dialogue.values():
			lines = value if isinstance(value, list) else [value]
			for line in lines:
				if isinstance(line, str) and line:
					l10n[(line, "Mob Dialogue")] = ""
	return l10n

def parse_team_messages():
	"""Collects team action messages that are assembled in C++ rather than
	passed through Localize literals. They use the "Team" localization context
	at runtime, so the extractor has to read the same helpers."""
	l10n = {}
	patterns = [
		re.compile(r'SetTeamError\([^,]+,\s*[^,]+,\s*"((?:[^"\\]|\\.)*)"'),
		re.compile(r'SendSystemChat\([^,]+,\s*"Team",\s*"((?:[^"\\]|\\.)*)"'),
	]
	for path in ("src/game/server/team.cpp", "src/game/server/gamecontroller.cpp"):
		if not os.path.isfile(path):
			continue
		source = open(path, encoding='utf-8').read()
		for pattern in patterns:
			for match in pattern.finditer(source):
				if match.group(1):
					l10n[(match.group(1), "Team")] = ""
	return l10n

def parse_script_chat():
	"""Collects player-visible chat strings written by Python scripts.

	The runtime localizes Player.chat(text, context); the same pair has to be
	present in the language files, so extract it from the script sources."""
	l10n = {}
	scripts_dir = "datasrc/scripts"
	if not os.path.isdir(scripts_dir):
		return l10n

	for root, dirs, files in os.walk(scripts_dir):
		dirs[:] = sorted(d for d in dirs if d != "__pycache__")
		for filename in sorted(files):
			if not filename.endswith(".py"):
				continue
			with open(os.path.join(root, filename), encoding='utf-8') as f:
				source = f.read()
			for match in SOURCE_SCRIPT_CHAT_RE.finditer(source):
				l10n[(match.group(1), match.group(2))] = ""
	return l10n

def parse_source():
	l10n_client = defaultdict(lambda: str)
	l10n_server = defaultdict(lambda: str)

	def add_match(match, l10n):
		str_ = match.group('str').decode()
		ctxt = match.group('ctxt')
		if ctxt is not None:
			ctxt = ctxt.decode()
		l10n[(str_, ctxt)] = ""

	def process_line(line, l10n):
		for match in SOURCE_LOCALIZE_RE.finditer(line):
			add_match(match, l10n)
		for match in SOURCE_LOCALIZE_FORMAT_RE.finditer(line):
			add_match(match, l10n)
		for match in SOURCE_SEND_SYSTEM_CHAT_RE.finditer(line):
			add_match(match, l10n)

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
	# A key can move between sections over time (an item description that was
	# once extracted from the server sources, for example). Keep the old
	# translation per section and also build a global fallback, so a move does
	# not drop the text.
	old_by_section = {}
	old_translations = {}
	for old_key in (JSON_KEY_CLIENT, JSON_KEY_SERVER, JSON_KEY_ITEM):
		section = {}
		for entry in old_l10n_data.get(old_key, []):
			key = (entry[JSON_KEY_OR], entry.get(JSON_KEY_CTXT))
			if entry[JSON_KEY_TR]:
				section.setdefault(key, entry[JSON_KEY_TR])
				old_translations.setdefault(key, entry[JSON_KEY_TR])
		old_by_section[old_key] = section

	def merge_old(src_key, l10n_src):
		"""Keep an existing translation only if its key is still present in the
		newly extracted source for that section."""
		translations = l10n_src.copy()
		for key, translation in old_by_section.get(src_key, {}).items():
			if key in translations and not translations[key]:
				translations[key] = translation
		for key, translation in old_translations.items():
			if key in translations and not translations[key]:
				translations[key] = translation
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
	# a language's display name is metadata, not extracted from the source
	if old_l10n_data.get(JSON_KEY_NAME):
		result[JSON_KEY_NAME] = old_l10n_data[JSON_KEY_NAME]

	for key in (JSON_KEY_CLIENT, JSON_KEY_SERVER, JSON_KEY_ITEM):
		# sort by context first, then by the original string within a context
		result[key].sort(key=lambda entry: (entry.get(JSON_KEY_CTXT) or "", entry[JSON_KEY_OR]))

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
	# the euclidean world names are data, not source literals: the menu looks
	# them up by floor name, so extract them from the world configs
	l10n_server.update(parse_world_names())
	# tips also live in a json file and are looked up by their text
	l10n_server.update(parse_tips())
	# container display names are data looked up with "Container Name"
	l10n_server.update(parse_container_names())
	# dialogue lines from mob/NPC json are looked up with "Mob Dialogue"
	l10n_server.update(parse_mob_dialogue())
	# team action messages assembled by helper calls
	l10n_server.update(parse_team_messages())
	# player-visible chat text written by Python scripts
	l10n_server.update(parse_script_chat())

	# item names, descriptions and types live in their own "item strings" section;
	# categories are folder names, so they are collected the same way
	l10n_item = parse_item_names()
	l10n_item.update(parse_category_folders("datasrc/craft", CRAFT_CATEGORY_CTXT))
	l10n_item.update(parse_category_folders("datasrc/items", ITEM_CATEGORY_CTXT))

	for filename in os.listdir("datasrc/languages"):
		try:
			if (os.path.splitext(filename)[1] == ".json"
					and filename != "index.json"):
				filename = "datasrc/languages/" + filename
				write_languagefile(filename, l10n_client, l10n_server, l10n_item, load_languagefile(filename))
		except Exception as e:
			print("Failed on {0}, re-raising for traceback".format(filename))
			raise
