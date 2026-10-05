from pathlib import Path
import shutil
import xml.etree.ElementTree as ET


root = Path("build/ios/UnFalsus")
storyboard = root / "Launch Screen.storyboard"
assets = root / "Images.xcassets" / "SplashImage.imageset"

tree = ET.parse(storyboard)
document = tree.getroot()
removed_ids = set()

for parent in document.iter():
    for child in list(parent):
        if child.tag == "imageView" and child.get("image") == "SplashImage":
            view_id = child.get("id")
            if view_id:
                removed_ids.add(view_id)
            parent.remove(child)

for parent in document.iter():
    for child in list(parent):
        if child.tag == "constraint" and (
            child.get("firstItem") in removed_ids or child.get("secondItem") in removed_ids
        ):
            parent.remove(child)

ET.indent(tree, space="  ")
tree.write(storyboard, encoding="utf-8", xml_declaration=True)
shutil.rmtree(assets, ignore_errors=True)

if "SplashImage" in storyboard.read_text(encoding="utf-8") or assets.exists():
    raise SystemExit("Godot SplashImage was not removed from the iOS launch screen")
