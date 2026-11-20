# carve-library

The Worker that serves Carve DAW's downloadable sound library out of the
`carve-library` R2 bucket, at <https://carve-library.hashrock.workers.dev>.

| Path | What |
| --- | --- |
| `/v1/library.json` | The list the app reads: each pack's version, sizes, URL and SHA-256 |
| `/v1/packs/<id>/<version>/<id>-<version>.zip` | A pack: `README-Carve.txt` and the author's folder, unchanged |
| `/v1/packs/<id>/<version>/README.txt` | Its credits and the author's terms, UTF-8, readable before downloading |

GET and HEAD only, with Range and ETag. Pack files are immutable; a new
version is a new path, and `library.json` (cached 5 minutes) points at it.

## Packs

| id | Pack | Author | Terms |
| --- | --- | --- | --- |
| `adachi-rei` | 足立レイ 3.5.0 (UTAU) | みさいる | Redistribution and use in programs allowed; corporate commercial use needs permission |
| `koharune-ami` | 小春音アミ 単独音 3.00 (UTAU) | あみたろ | Redistribution allowed, notify the author afterwards |
| `koharune-ami-vcv` | 小春音アミ 連続音 1.10 (UTAU) | あみたろ | Same |
| `generaluser-gs` | GeneralUser GS 2.0.3 (SF2) | S. Christian Collins | Free to use and to include in software; host your own copy |

Each pack's entry in `packs.json` names where its terms are, and they are
copied into its README in full.

## Permission from the authors

Both UTAU authors were asked by email, before the library went up, whether
Carve DAW may serve their voicebanks from this mirror for download inside
the app. Both said yes. The conditions they named, and how they are met:

- **足立レイ** (みさいる, reply 2026-11-15). Serving it from our own mirror
  as an optional, one-button download is fine. Keep the readme and the rest
  of the distribution as they are: the zip holds the author's folder
  unchanged. Move to a new version when one comes out where practical,
  though 3.5.0 is fine for now. A tuned or otherwise modified copy has to
  say so in its readme and version name; nothing here is modified. If Carve
  ever becomes paid software with this as a default voice, ask again.
- **小春音アミ** (あみたろ, reply 2026-11-19). The way it was described is
  fine: readme included, a credit and a link to the official site in the
  app, the terms readable in the app (Sound Library window). That covers the
  other 小春音アミ UTAU voicebanks too, without asking again for each.
  **Once the app is public, tell her**, with the app's name, the developer
  name to show, the App Store URL, and when she may announce it (or that
  she should not).

The owner's replies to both said Carve is not to be paid for or carry ads,
and that a change of plan would be raised with them first (see
"Revenue limits" in `LICENSING.md`).

GeneralUser GS needs no permission: its licence allows inclusion in
software and asks only that you host your own copy, which this is.

## Publishing

    cd library-server && wrangler deploy        # the Worker (rarely)
    scripts/package-library.py                  # build into build-library/, check it
    scripts/package-library.py --upload         # build and put everything into R2

To add or update a pack: unpack the author's distribution, add or edit its
entry in `packs.json` (bump `version` for an update), then upload. The script
uploads `library.json` last, so the list never names a file that is not there.
The zips are deterministic: the same sources give the same SHA-256.
