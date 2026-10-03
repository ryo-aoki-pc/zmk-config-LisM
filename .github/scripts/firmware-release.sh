#!/usr/bin/env bash
# ファームウェアのビルドを GitHub のリリース (プレリリース) として公開する。
# zmk-config-{LisM,AroundFortyRB,KUKEY42,Pyuron,roBa} / zmk-keyboard-torabo-tsuki-lp / keyball /
# vial-qmk-kq-mini の 8 リポジトリで同じ内容にしている (直すときはすべてに入れる)。
#
#   bash .github/scripts/firmware-release.sh latest <ファイル...>  custom ブランチの最新 (タグ firmware-latest)
#   bash .github/scripts/firmware-release.sh custom <ファイル...>  custom のビルドの履歴 (firmware-custom-<sha7>)
#   bash .github/scripts/firmware-release.sh pr <ファイル...>      PR のビルド (firmware-pr-<番号>。push のたびに置き換える)
#
# custom と pr は、それぞれ新しい順に KEEP 件 (既定 30) を残し、古いリリースとタグを消す。
# zmk-config-keyboards の tools/flash.cmd は、リリースの一覧 (GitHub API) からビルドを選び、
#   https://github.com/<owner>/<repo>/releases/download/<タグ>/<ファイル>
# から認証なしでダウンロードする。リリースの本文の ```text ブロックと BUILD_INFO.txt の
# 「キー: 値」の行 (repository kind branch pr title head commit subject built run) は、
# そのツールが読む取り決めなので、キーの名前と形を変えないこと。
#
# 環境変数:
#   GH_TOKEN, GH_REPO (owner/repo), GITHUB_RUN_ID, GITHUB_SERVER_URL, GITHUB_SHA  (Actions が設定するもの)
#   BRANCH       ビルドしたブランチ (PR は head のブランチ)
#   HEAD_SHA     ブランチ (PR は head) のコミット
#   PR_NUMBER    PR の番号 (pr のみ)
#   PR_TITLE     PR のタイトル (pr のみ)
#   DESCRIPTION  中身の説明 (例: build.yaml の全ファームウェア)
#   KEEP         残す件数 (既定 30)
# shellcheck disable=SC2153  # BRANCH などは環境変数
set -euo pipefail
export LC_ALL=C.UTF-8

die() {
    echo "::error::$*" >&2
    exit 1
}

warn() {
    echo "::warning::$*"
}

need() {
    local name
    for name in "$@"; do
        [[ -n "${!name:-}" ]] || die "環境変数 ${name} がありません"
    done
}

# 1 行にして (改行・タブを空白に)、長すぎれば文字数で切る
one_line() {
    local text=${1//$'\r'/ } max=${2:-120}
    text=${text//$'\n'/ }
    text=${text//$'\t'/ }
    text=${text//\`\`\`/\'\'\'}
    if ((${#text} > max)); then
        text="${text:0:max-1}…"
    fi
    printf '%s' "$text"
}

# BUILD_INFO.txt から 1 つのキーの値を取り出す
info_get() {
    awk -v key="$2" 'index($0, key ": ") == 1 { print substr($0, length(key) + 3); exit }' "$1"
}

# BUILD_INFO.txt を書く。$1: 出力先、$2: custom / pr
cmd_info() {
    local out=$1 kind=$2 subject='' message=''
    need GH_REPO GITHUB_SHA GITHUB_RUN_ID GITHUB_SERVER_URL BRANCH HEAD_SHA
    if message=$(gh api "repos/${GH_REPO}/commits/${HEAD_SHA}" --jq '.commit.message' 2>/dev/null); then
        subject=$(one_line "${message%%$'\n'*}" 120)
    else
        warn "コミット ${HEAD_SHA::7} の件名を取得できませんでした"
    fi
    {
        echo "repository: ${GH_REPO}"
        echo "kind: ${kind}"
        echo "branch: ${BRANCH}"
        if [[ "$kind" == pr ]]; then
            need PR_NUMBER
            echo "pr: ${PR_NUMBER}"
            echo "title: $(one_line "${PR_TITLE:-}" 120)"
        fi
        echo "head: ${HEAD_SHA}"
        echo "commit: ${GITHUB_SHA}"
        echo "subject: ${subject}"
        echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "run: ${GITHUB_SERVER_URL}/${GH_REPO}/actions/runs/${GITHUB_RUN_ID}"
    } >"$out"
    cat "$out"
}

# このタグのリリースの ID (下書きも含む)
release_ids() {
    gh api --paginate "repos/${GH_REPO}/releases?per_page=100" --jq ".[] | select(.tag_name == \"$1\") | .id"
}

# タグをこのビルドのコミットへ付ける。タグは削除しない: GitHub はタグの削除を後から非同期に処理し、
# そのとき同じタグ名のリリースを下書きに戻すため、消してすぐ作り直すと、作ったばかりのリリースまで
# 下書きになり、ダウンロード URL が 404 になることがあった。
point_tag() {
    local tag=$1 sha
    shift
    for sha in "$@"; do
        if gh api "repos/${GH_REPO}/git/ref/tags/${tag}" --silent 2>/dev/null; then
            if gh api -X PATCH "repos/${GH_REPO}/git/refs/tags/${tag}" -f sha="${sha}" -F force=true --silent; then
                return 0
            fi
        elif gh api -X POST "repos/${GH_REPO}/git/refs" -f ref="refs/tags/${tag}" -f sha="${sha}" --silent; then
            return 0
        fi
        warn "タグ ${tag} を ${sha::7} に付けられませんでした"
    done
    gh api "repos/${GH_REPO}/git/ref/tags/${tag}" --silent 2>/dev/null ||
        die "タグ ${tag} を作れませんでした"
    warn "タグ ${tag} は前のコミットのままです (中身のコミットは BUILD_INFO.txt を参照)"
}

# $1: タグ、$2: BUILD_INFO.txt、残り: 置くファイル
cmd_publish() {
    local tag=$1 info=$2 kind commit head branch short title notes body prev url i id
    shift 2
    need GH_REPO GITHUB_RUN_ID GITHUB_SERVER_URL
    [[ $# -gt 0 ]] || die "置くファイルがありません"
    local f
    for f in "$@"; do
        [[ -f "$f" ]] || die "ファイルがありません: $f"
    done

    kind=$(info_get "$info" kind)
    commit=$(info_get "$info" commit)
    head=$(info_get "$info" head)
    branch=$(info_get "$info" branch)
    short=${commit::7}
    notes=$(mktemp)
    case "$tag" in
        firmware-latest)
            title="Latest firmware (${branch} @ ${short})"
            echo "custom ブランチの最新ビルド (${DESCRIPTION:-ファームウェア})。push のたびに自動で置き換わります。" >"$notes"
            ;;
        firmware-custom-*)
            title="custom @ ${short}: $(info_get "$info" subject)"
            echo "custom ブランチのビルド (${DESCRIPTION:-ファームウェア})。新しいものから ${KEEP:-30} 件を残し、古いものは自動で削除します。" >"$notes"
            ;;
        firmware-pr-*)
            title="PR #$(info_get "$info" pr) @ ${head::7}: $(info_get "$info" title)"
            echo "PR #$(info_get "$info" pr) (${branch}) のビルド (${DESCRIPTION:-ファームウェア})。PR をマージした状態のコミット (${short}) をビルドしています。PR に push するたびに置き換わり、PR のビルドは新しいものから ${KEEP:-30} 件を残します。" >"$notes"
            ;;
        *)
            die "対応していないタグです: ${tag}"
            ;;
    esac
    {
        echo
        echo '```text'
        cat "$info"
        echo '```'
    } >>"$notes"

    # 後から始まった run のビルドが既に公開されていたら、置き換えない
    # (PR に続けて push したときなどに、先に終わった新しいビルドを古いビルドで上書きしない)
    if body=$(gh api "repos/${GH_REPO}/releases/tags/${tag}" --jq '.body' 2>/dev/null); then
        prev=$(awk -F'/actions/runs/' '/^run: / { split($2, a, /[^0-9]/); print a[1]; exit }' <<<"$body")
        if [[ -n "$prev" && "$prev" -gt "$GITHUB_RUN_ID" ]]; then
            echo "::notice::${tag} は新しい run (${prev}) のビルドが公開済みなので、置き換えません"
            return 0
        fi
    fi

    if [[ "$kind" == pr && "$head" != "$commit" ]]; then
        point_tag "$tag" "$commit" "$head"
    else
        point_tag "$tag" "$commit"
    fi

    # 下書きに戻されたものも含めて、このタグのリリースをすべて消す (タグは消さない)
    for id in $(release_ids "$tag"); do
        gh api -X DELETE "repos/${GH_REPO}/releases/${id}" --silent || warn "リリース ${id} を削除できませんでした"
    done

    # プレリリースなので releases/latest や v* タグのリリース (release.yml) には影響しない
    gh release create "$tag" --verify-tag --prerelease --title "$title" --notes-file "$notes" "$@"
    rm -f "$notes"

    # 認証なしで今回のビルドを取得できるかを確かめる。下書きになっていたら公開し直し、
    # それでも取得できなければジョブを失敗させる (書き込みツールが 404 になる状態を見逃さない)
    url="${GITHUB_SERVER_URL}/${GH_REPO}/releases/download/${tag}/BUILD_INFO.txt"
    for i in 1 2 3 4 5 6; do
        body=$(curl -fsSL "$url" 2>/dev/null || true)
        if grep -qx "commit: ${commit}" <<<"$body"; then
            echo "OK: ${url}"
            return 0
        fi
        for id in $(gh api --paginate "repos/${GH_REPO}/releases?per_page=100" \
            --jq ".[] | select(.tag_name == \"${tag}\" and .draft) | .id"); do
            warn "${tag} リリース (${id}) が下書きになっているため公開し直します"
            gh api -X PATCH "repos/${GH_REPO}/releases/${id}" -F draft=false --silent || true
        done
        if [[ $i -lt 6 ]]; then
            sleep 10
        fi
    done
    die "${url} から今回のビルド (${short}) を取得できません"
}

# $1: firmware-custom- / firmware-pr-、$2: 残す件数
# 新しい順 (作成日時) に $2 件のタグを残し、それより古いタグのリリースを消す。
# タグは、リリースを消したものだけを消す (公開の途中でまだリリースの無いタグは消さない)。
cmd_prune() {
    local prefix=$1 keep=$2 list old tag id left
    need GH_REPO
    [[ "$prefix" =~ ^firmware-(custom|pr)-$ ]] || die "消してよいタグの接頭辞ではありません: ${prefix}"
    [[ "$keep" =~ ^[0-9]+$ && "$keep" -ge 1 ]] || die "残す件数が正しくありません: ${keep}"

    list=$(gh api --paginate "repos/${GH_REPO}/releases?per_page=100" \
        --jq ".[] | select(.tag_name | startswith(\"${prefix}\")) | [.tag_name, .created_at, (.id | tostring)] | @tsv")
    [[ -n "$list" ]] || return 0
    old=$(sort -t $'\t' -k2,2r <<<"$list" | awk -F'\t' -v k="$keep" '!($1 in seen) { seen[$1] = ++n } seen[$1] > k')
    [[ -n "$old" ]] || return 0

    while IFS=$'\t' read -r tag _ id; do
        echo "古いビルドを削除します: ${tag} (リリース ${id})"
        gh api -X DELETE "repos/${GH_REPO}/releases/${id}" --silent || warn "リリース ${id} を削除できませんでした"
    done <<<"$old"

    # 消している間に同じタグで公開し直されていたら (古い PR に push したときなど)、タグは残す
    left=$(gh api --paginate "repos/${GH_REPO}/releases?per_page=100" --jq '.[].tag_name')
    while read -r tag; do
        if grep -qxF "$tag" <<<"$left"; then
            echo "${tag} は公開し直されたので、タグを残します"
            continue
        fi
        gh api -X DELETE "repos/${GH_REPO}/git/refs/tags/${tag}" --silent || warn "タグ ${tag} を削除できませんでした"
    done < <(cut -f1 <<<"$old" | sort -u)
}

main() {
    local cmd=${1:-}
    shift || true
    case "$cmd" in
        info) cmd_info "$@" ;;
        publish) cmd_publish "$@" ;;
        prune) cmd_prune "$@" ;;
        latest)
            cmd_info BUILD_INFO.txt custom
            cmd_publish firmware-latest BUILD_INFO.txt "$@" BUILD_INFO.txt
            ;;
        custom)
            need GITHUB_SHA
            cmd_info BUILD_INFO.txt custom
            cmd_publish "firmware-custom-${GITHUB_SHA::7}" BUILD_INFO.txt "$@" BUILD_INFO.txt
            cmd_prune firmware-custom- "${KEEP:-30}"
            ;;
        pr)
            need PR_NUMBER
            [[ "$PR_NUMBER" =~ ^[0-9]+$ ]] || die "PR の番号が正しくありません: ${PR_NUMBER}"
            cmd_info BUILD_INFO.txt pr
            cmd_publish "firmware-pr-${PR_NUMBER}" BUILD_INFO.txt "$@" BUILD_INFO.txt
            cmd_prune firmware-pr- "${KEEP:-30}"
            ;;
        *)
            die "使い方: firmware-release.sh latest|custom|pr <ファイル...>"
            ;;
    esac
}

main "$@"
