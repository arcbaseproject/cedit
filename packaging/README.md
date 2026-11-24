# Packaging

Each directory builds the tagged release and runs `make test`. All were
built and installed in a clean container or VM for 1.0.1 (1.2.0 checksums updated, not rebuilt yet).

| Directory | Target | Status |
|-----------|--------|--------|
| `aur/` | Arch AUR (`cedit`, `cedit-git`) | ready; waiting for AUR registration |
| `rpm/` | Fedora Copr, openSUSE OBS | ready |
| `debian/` | OBS, Launchpad PPA, Debian mentors | ready (Debian needs an ITP bug) |
| `alpine/` | Alpine aports `testing` | ready |
| `gentoo/` | Gentoo GURU | ready |
| `nixpkgs/` | `pkgs/by-name/ce/cedit/package.nix` | ready; needs a maintainer entry |
| `homebrew/` | copy of the formula in arcbaseproject/homebrew-tap | live |

## New release checklist

1. Bump `VERSION`, `src/main.c` and `docs/cedit.1`; tag `vX.Y.Z`; publish a release.
2. Download the tarball once and compute sha256 and sha512.
3. Update each file: `pkgver`/`Version`/`version`, checksums, and the
   changelog in `rpm/` and `debian/changelog`.
4. AUR: `makepkg --printsrcinfo > .SRCINFO`. Gentoo: rename the ebuild,
   run `ebuild ... manifest`. nixpkgs: new `hash` from
   `nix flake prefetch github:arcbaseproject/cedit/vX.Y.Z`.
5. Push the formula to the tap.
