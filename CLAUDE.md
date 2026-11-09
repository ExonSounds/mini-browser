# mini-browser

C++ / GTK 4 / WebKitGTK 6.0 browser. Build with `make`; see README.md.

## Publishing rule (from the project owner)

Every modification must be published: after any change to the project, build
it (`make`), then commit and push to GitHub (`origin`, branch `main`,
https://github.com/ExonSounds/mini-browser) in the same task. Don't leave
changes uncommitted or unpushed.

- Commit author for this repo is set locally to the GitHub noreply address;
  don't commit with a personal email.
- Never commit the built `browser` binary (it's in `.gitignore`).
