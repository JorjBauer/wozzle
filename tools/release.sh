#!/bin/sh
# Publish dist/release/ (the tools) and gui/dist/ (the app) as a GitHub
# release. `make release` runs it twice:
#
#   tools/release.sh check   the safety checks only, before building, so a
#                            version you forgot to bump fails in seconds
#   tools/release.sh         the checks again, then the upload, after
#                            `make dist`
#
# A release holds whatever the computer that made it could build (a Mac
# builds the Mac app and tools). To put another platform's files on the
# same release, build them there with `make test dist` and run:
#
#   tools/release.sh add     upload this computer's files to the existing
#                            release for this version
#
# That's what .github/workflows/build.yml does for Linux and Windows once
# a release is published. It sets RELEASE_YES=1, since there's nobody
# there to answer "Publish?".

set -e

REMOTE=${GITHUB_REMOTE:-origin}
VERSION=$(sed -n 's/^version=//p' library.properties)
TAG="v$VERSION"
MODE=${1:-publish}

die() {
  echo "release: $*" >&2
  exit 1
}

case "$MODE" in
  check | publish | add) ;;
  *) die "usage: tools/release.sh [check | add]" ;;
esac

[ -n "$VERSION" ] || die "no version= line in library.properties"
make -s version >/dev/null || die "make version couldn't write the version from library.properties"
git diff --quiet -- src/wozversion.h gui/package.json ||
  die "src/wozversion.h or gui/package.json didn't match library.properties; make version has updated them, so commit that"

command -v gh >/dev/null || die "needs the GitHub CLI: brew install gh, then gh auth login"
gh auth status >/dev/null 2>&1 || die "gh isn't logged in: run gh auth login"

URL=$(git remote get-url "$REMOTE" 2>/dev/null) || die "no git remote called $REMOTE (set GITHUB_REMOTE to use another)"
case "$URL" in
  git@github.com:*/* | https://github.com/*/* | ssh://git@github.com/*/*) ;;
  *) die "$REMOTE ($URL) doesn't look like a GitHub repo" ;;
esac
REPO=$(echo "$URL" | sed -E 's#^(git@github\.com:|https://github\.com/|ssh://git@github\.com/)##; s#\.git$##')

# First, because it's the likeliest mistake: a release for this version
# already exists, so the version wasn't bumped. (Adding to a release wants
# the opposite.) Anything other than a clean "not found" (no network, bad
# login) stops here too, rather than passing.
if out=$(gh release view "$TAG" --repo "$REPO" 2>&1 >/dev/null); then
  [ "$MODE" = add ] ||
    die "$REPO already has a release $TAG. Did you forget to bump the version in library.properties?"
else
  case "$out" in
    *"not found"*)
      [ "$MODE" != add ] || die "$REPO has no release $TAG to add to; make release makes it" ;;
    *) die "couldn't ask GitHub about $TAG: $out" ;;
  esac
fi

[ -z "$(git status --porcelain)" ] || die "the working tree has uncommitted changes"

HEAD=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)

# The release is of HEAD, so GitHub has to have that commit already.
git fetch -q "$REMOTE"
git branch -r --contains HEAD | grep -q "^ *$REMOTE/" ||
  die "$SHORT isn't on $REMOTE yet; push it first"

# The tag comes from library.properties. It's made here if it doesn't
# exist yet; if it does, locally or on GitHub, it has to be HEAD already.
LOCAL=$(git rev-parse -q --verify "refs/tags/$TAG^{commit}" || true)
[ -z "$LOCAL" ] || [ "$LOCAL" = "$HEAD" ] ||
  die "$TAG is already $(git rev-parse --short "$LOCAL"), not HEAD ($SHORT). Bump the version in library.properties for a new release."
# The peeled line (^{}) is the commit an annotated tag points at.
REMOTE_TAG=$(git ls-remote --tags "$REMOTE" "refs/tags/$TAG" "refs/tags/$TAG^{}" | awk '{print $1}' | tail -1)
[ -z "$REMOTE_TAG" ] || [ "$REMOTE_TAG" = "$HEAD" ] ||
  die "$TAG on $REMOTE isn't HEAD ($SHORT). Bump the version in library.properties for a new release."
[ "$MODE" != add ] || [ -n "$REMOTE_TAG" ] ||
  die "$TAG isn't on $REMOTE, so the release there isn't of this commit"

if [ "$MODE" = check ]; then
  echo "release: $TAG of $SHORT looks ready to publish to $REPO"
  exit 0
fi

# Whatever make dist left for this version: the app and the tools, at least
FILES=$(ls gui/dist/Wozzle-"$VERSION"-* dist/release/wozzle-tools-"$VERSION"-* 2>/dev/null || true)
echo "$FILES" | grep -q "/Wozzle-$VERSION-" || die "no app in gui/dist (make dist builds it)"
echo "$FILES" | grep -q "/wozzle-tools-$VERSION-" || die "no tools in dist/release (make dist builds them)"

if [ "$MODE" = add ]; then
  echo
  echo "About to add to $TAG at https://github.com/$REPO/releases"
else
  # Release notes: which file is which, then the README's first-launch help
  NOTES=dist/release/notes.md
  {
    echo "Which file:"
    echo
    echo "- \`Wozzle-$VERSION-<system>-<chip>\`: the app"
    echo "- \`wozzle-tools-$VERSION-<system>-<chip>\`: the command line tools (wozzle, wozit, wozmosis, and wozfuse where there's FUSE)"
    echo
    echo "darwin is macOS. A Mac file marked arm64 is for Apple Silicon; universal runs on Apple Silicon and Intel."
    echo
    echo "win32 is Windows, where the app is a \`-setup.exe\` installer and the tools are a .zip. linux is Linux, where the app is an .AppImage or a .deb."
    echo
    echo "GitHub builds the Linux and Windows files after the release is made, so they show up a few minutes after the Mac ones."
    echo
    # From that paragraph to the next heading. The README's headings are
    # underlined, so a line is only known not to be one by the line after.
    awk '
      /^None of .* signed/ { on = 1 }
      !on { next }
      /^(-+|=+)$/ { stop = 1; exit }
      { if (have) print prev; prev = $0; have = 1 }
      END { if (have && !stop) print prev }
    ' README.md
  } > "$NOTES"
  grep -q '^None of .* signed' "$NOTES" ||
    die "couldn't find the first-launch section in README.md (a paragraph starting \"None of ... signed\")"
  echo
  echo "About to publish $TAG to https://github.com/$REPO/releases"
  [ -n "$REMOTE_TAG" ] || echo "  and tag $SHORT as $TAG on GitHub$([ -n "$LOCAL" ] || echo ' and here')"
fi
for f in $FILES; do
  echo "  $f ($(du -h "$f" | cut -f1 | tr -d ' '))"
done
[ "$MODE" = add ] || echo "Notes: $NOTES"
if [ "$RELEASE_YES" = 1 ]; then
  answer=y
else
  printf 'Publish? [y/N] '
  read -r answer
fi
case "$answer" in
  y|Y|yes) ;;
  *) die "not published" ;;
esac

if [ "$MODE" = add ]; then
  # --clobber replaces a file of the same name, for a rebuilt platform
  gh release upload "$TAG" $FILES --repo "$REPO" --clobber
  echo "Added to $TAG."
  exit 0
fi

# --target makes GitHub create the tag at HEAD if it isn't there yet.
gh release create "$TAG" $FILES --repo "$REPO" --target "$HEAD" \
  --title "Wozzle $VERSION" --notes-file "$NOTES"
[ -n "$LOCAL" ] || git tag "$TAG" "$HEAD"
echo "Tagged $SHORT as $TAG. To put the tag on another remote too: git push jdo $TAG"
