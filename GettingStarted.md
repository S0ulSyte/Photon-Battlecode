# Getting Started (CAMBC, C++)

Source: https://2026a.battlecode.co.uk/game/docs/first-bot — saved from the handbook page (5 min read, current game rules). Event: cambc2026a.

## 1. Get the CLI

Download the portable CAMBC CLI for your platform from the CLI page (https://2026a.battlecode.co.uk/game/download), or install it with pip. Put the executable on PATH so you can run `cambc` from your terminal.

Install or update with pip:

```bash
python -m pip install --upgrade cambc
cambc --version
```

The CLI downloads the pinned WASI SDK, CMake and Ninja when needed. You do not need to install a C++ build toolchain yourself.

## 2. Create a workspace

Start the interactive setup:

```bash
cambc init
```

- Choose where to create the workspace; the default is your current directory.
- Nothing is selected initially. Use the arrow keys to find C++, press Space to select it, then press Enter. You must select at least one language.
- Choose to include starter bots, then select C++ with Space and press Enter. It drafts champions, uses basic attacks, farms camps, banks gems and buys power shards.
- With shell integration enabled, init enters the workspace automatically. Otherwise, change into the printed workspace directory before running the remaining commands.

Setup downloads the currently published map pool into `maps/`. If the platform has no maps published, no maps are downloaded. Once maps are available, refresh them with the command below or rerun `cambc init`.

Refresh the map pool:

```bash
cambc maps pull
```

**Refreshing an existing workspace.** Rerun `cambc init` in the workspace to refresh the SDK and repair managed files. Existing bot source is preserved; create a new bot to get the latest starter code.

## 3. Create a new bot

Create another C++ starter:

```bash
cambc new my-bot --lang cpp
```

You can also run `cambc new` without arguments and answer its prompts.

Open the workspace in VS Code with clangd installed. The CLI generates `compile_commands.json` and the editor configuration for your bot automatically, and refreshes them after builds.

Build explicitly:

```bash
cambc build my-bot
```

Build artefacts stay in `bots/my-bot/.cambc/build/`. The CLI manages compilers and caches; do not put generated binaries in your submission.

## 4. Run and watch a match

Run commands from your CAMBC workspace. Bot names resolve to projects in `bots/`; changed bots are built automatically, with artefacts kept in each bot's `.cambc/build/` directory.

Play against the original starter:

```bash
cambc run my-bot starter-cpp --view
```

- The terminal reports match progress.
- The first bot is team A (blue), and the second is team B (red).
- Without `--map`, the CLI uses the first `.map26a` file in `maps/` alphabetically.
- `--view` opens the replay after the match. Each run saves a timestamped `.repl26a` replay bundle in `replays/` in the project by default; `--out-dir` (or `-o`) selects a different destination.

By default, the match runs until the game finishes. Use `--rounds 50` for a short smoke test, but use full matches to evaluate a strategy. Leave `--fuel` at its default for normal testing; fuel limits computation, not wall-clock time.

## 5. Test different maps

Refresh the platform's published map pool with `cambc maps pull` or `cambc init`. Custom and edited maps are preserved when refreshing.

Choose a downloaded map:

```bash
cambc run my-bot starter-cpp --map maps/<downloaded-map>.map26a --view
```

Replace `<downloaded-map>` with a real filename from `maps/`. Repeat on different maps to check routes, capture decisions and altar access.

## 6. Inspect a replay

Open the replay path printed after a run:

```bash
cambc view replays/<replay-bundle>.repl26a
```

Each replay bundle contains the replay with its map and both bots' stdout and debug logs. Use the Visualiser to inspect the logs alongside the timeline; stdout files contain binary timeline markers. You can also load the bundle on the platform's Visualiser page.

- Look for idle turns, failed paths, unfinished captures and lost gems.
- Compare the bot's logged decisions with what its champion could see at that moment.

Export final statistics:

```bash
cambc run my-bot starter-cpp --statistics replays/stats.json
```

`--statistics` writes final game statistics as JSON and requires the game to finish; a shortened match may not have a final result.

## 7. Run several matches

Create `matches.txt` in your workspace. Each line has four paths: first bot directory, second bot directory, map, and output replay. Paths are relative to that file; quote paths containing spaces. Replace the example map filenames with real ones.

matches.txt:

```
bots/my-bot bots/starter-cpp maps/<map-one>.map26a replays/test-one.repl26a
bots/my-bot bots/starter-cpp maps/<map-two>.map26a replays/test-two.repl26a
```

Run up to two matches at once:

```bash
cambc run-batch matches.txt --jobs 2
```

The terminal shows progress for each match. Choose unused output filenames: batch runs reject existing replay or diagnostic files. `--timeout` sets a wall-clock safety limit in seconds per match; it does not change turn fuel.

## 8. Submit your bot

Sign in to the platform, register for the event, and create or join a team. You can submit directly from the CLI or upload a source ZIP in your browser.

Submit directly:

```bash
cambc login
cambc submit my-bot
```

Login opens the browser for approval. The CLI securely stores a seven-day session and asks you to sign in again when needed. Submit uploads source to your current team.

Or create an uploadable ZIP:

```bash
cambc bundle my-bot -o submissions
```

Upload `submissions/my-bot.zip` on the platform's Submissions page. Bundle uses the same source selection as submit, excludes build artefacts, and does not require login. Use `--output path/to/bot.zip` if you want an exact filename; existing files are not overwritten.

Follow compilation status on the Submissions page, then open completed matches and replays from the platform.

## Related pages

- Bot API reference (C++ SDK): https://2026a.battlecode.co.uk/game/api (see BotAPICpp.md)
- Shared memory: https://2026a.battlecode.co.uk/game/docs/shared-memory
- Glossary: https://2026a.battlecode.co.uk/game/docs/glossary
