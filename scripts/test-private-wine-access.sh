#!/usr/bin/env bash
set -euo pipefail

REPO="${WINE_GIT_REPO:-gamesir123/wine-proton-macos}"
BRANCH="${WINE_GIT_BRANCH:-proton-11.0-macos}"
TOKEN="${WINE_ACCESS_TOKEN:-}"

usage() {
  cat <<'EOF'
usage: test-private-wine-access.sh [--token-stdin]

Environment:
  WINE_ACCESS_TOKEN  GitHub token to test. Prefer --token-stdin to avoid shell history.
  WINE_GIT_REPO      owner/name repository, default: gamesir123/wine-proton-macos
  WINE_GIT_BRANCH    branch to test, default: proton-11.0-macos

Examples:
  printf '%s' "$TOKEN" | scripts/test-private-wine-access.sh --token-stdin
  WINE_ACCESS_TOKEN="$TOKEN" scripts/test-private-wine-access.sh
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "${1:-}" == "--token-stdin" ]]; then
  TOKEN="$(cat)"
  TOKEN="${TOKEN//$'\r'/}"
  TOKEN="${TOKEN//$'\n'/}"
  shift
fi

if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

if [[ -z "${TOKEN}" ]]; then
  echo "error: WINE_ACCESS_TOKEN is empty; pass --token-stdin or set the environment variable" >&2
  exit 2
fi

TOKEN_KIND="unknown"
case "${TOKEN}" in
  ghp_*) TOKEN_KIND="classic" ;;
  github_pat_*) TOKEN_KIND="fine-grained" ;;
esac

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/dxmt-wine-access.XXXXXX")"
cleanup() {
  rm -rf "${tmp_dir}"
}
trap cleanup EXIT

api_headers="${tmp_dir}/api-headers.txt"
api_body="${tmp_dir}/api-body.json"
branch_headers="${tmp_dir}/branch-headers.txt"
branch_body="${tmp_dir}/branch-body.json"
curl_status() {
  local url="$1"
  local headers="$2"
  local body="$3"
  curl -sS \
    --connect-timeout 15 \
    --max-time 60 \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    -D "${headers}" \
    -o "${body}" \
    -w '%{http_code}' \
    "${url}"
}

echo "[dxmt-test] repo=${REPO}"
echo "[dxmt-test] branch=${BRANCH}"
echo "[dxmt-test] token kind=${TOKEN_KIND}"

echo "[dxmt-test] checking repository API"
repo_status="$(curl_status "https://api.github.com/repos/${REPO}" "${api_headers}" "${api_body}")"
echo "[dxmt-test] repo api status=${repo_status}"
case "${repo_status}" in
  200)
    python3 - "${api_body}" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fp:
    data = json.load(fp)
print(f"[dxmt-test] repo full_name={data.get('full_name')}")
print(f"[dxmt-test] repo private={data.get('private')}")
print(f"[dxmt-test] default_branch={data.get('default_branch')}")
PY
    ;;
  401|403|404)
    python3 - "${api_body}" <<'PY' || true
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fp:
    data = json.load(fp)
print(f"[dxmt-test] api message={data.get('message')}")
PY
    if [[ "${repo_status}" == "404" && "${TOKEN_KIND}" == "fine-grained" ]]; then
      echo "error: this looks like a fine-grained PAT. GitHub does not grant fine-grained PAT access to personal-account repositories where you are only an invited collaborator." >&2
      echo "error: create a classic PAT with the repo scope, or ask the repository owner for a deploy key/GitHub App token." >&2
    fi
    echo "error: token cannot access repository metadata" >&2
    exit 1
    ;;
  *)
    cat "${api_body}" >&2
    echo "error: unexpected repository API status ${repo_status}" >&2
    exit 1
    ;;
esac

echo "[dxmt-test] checking branch API"
branch_status="$(curl_status "https://api.github.com/repos/${REPO}/branches/${BRANCH}" "${branch_headers}" "${branch_body}")"
echo "[dxmt-test] branch api status=${branch_status}"
if [[ "${branch_status}" != "200" ]]; then
  python3 - "${branch_body}" <<'PY' || true
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fp:
    data = json.load(fp)
print(f"[dxmt-test] api message={data.get('message')}")
PY
  echo "error: token can access repo metadata but branch was not readable" >&2
  exit 1
fi

python3 - "${branch_body}" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fp:
    data = json.load(fp)
commit = data.get("commit") or {}
print(f"[dxmt-test] branch commit={commit.get('sha')}")
PY

echo "[dxmt-test] git ls-remote"
GIT_TERMINAL_PROMPT=0 \
git \
  -c protocol.version=2 \
  -c credential.helper= \
  -c "http.https://github.com/.extraHeader=AUTHORIZATION: Bearer ${TOKEN}" \
  ls-remote --heads "https://github.com/${REPO}.git" "${BRANCH}" |
  sed 's/^/[dxmt-test] /'

echo "[dxmt-test] ok"
