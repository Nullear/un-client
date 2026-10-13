import base64
import json
import subprocess
from pathlib import Path


def api(method, endpoint, payload=None):
    command = ["gh", "api", "--method", method, endpoint]
    data = None
    if payload is not None:
        command.extend(["--input", "-"])
        data = json.dumps(payload).encode()
    result = subprocess.run(command, input=data, capture_output=True, check=True)
    return json.loads(result.stdout)


repo = "repos/Nullear/un-client"
latest = api("GET", repo + "/git/ref/heads/main")["object"]["sha"]
tree_sha = api("GET", repo + "/git/commits/" + latest)["tree"]["sha"]
changed = subprocess.run(
    ["git", "diff", "HEAD", "--name-only", "-z"], capture_output=True, check=True
).stdout
untracked = subprocess.run(
    ["git", "ls-files", "--others", "--exclude-standard", "-z"], capture_output=True, check=True
).stdout
paths = sorted({
    path.decode()
    for path in changed.split(b"\0") + untracked.split(b"\0")
    if path
    and b"__pycache__" not in path.split(b"/")
    and not path.endswith(b".pyc")
    and path not in {b"build_release.bat", b"icon_192_backup.png", b"tools/upload_git_database.py"}
})
if not paths:
    raise SystemExit("No changed files to publish")
entries = []
for path in paths:
    blob = api("POST", repo + "/git/blobs", {
        "content": base64.b64encode(Path(path).read_bytes()).decode(), "encoding": "base64"})
    entries.append({"path": path, "mode": "100644", "type": "blob", "sha": blob["sha"]})
tree = api("POST", repo + "/git/trees", {"base_tree": tree_sha, "tree": entries})
commit = api("POST", repo + "/git/commits", {
    "message": "Build iOS IPA from latest client changes", "tree": tree["sha"], "parents": [latest]})
api("PATCH", repo + "/git/refs/heads/main", {"sha": commit["sha"]})
print(commit["sha"])
