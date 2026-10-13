extends SceneTree

const SNAPSHOT := "user://konzetsu_tasks.cfg"
var saved_snapshot: PackedByteArray
var snapshot_existed := false
var failed := false

func _initialize() -> void:
    snapshot_existed = FileAccess.file_exists(SNAPSHOT)
    if snapshot_existed:
        saved_snapshot = FileAccess.get_file_as_bytes(SNAPSHOT)
    call_deferred("run")

func finish(code: int) -> void:
    if snapshot_existed:
        var file := FileAccess.open(SNAPSHOT, FileAccess.WRITE)
        file.store_buffer(saved_snapshot)
        file.close()
    elif FileAccess.file_exists(SNAPSHOT):
        DirAccess.remove_absolute(ProjectSettings.globalize_path(SNAPSHOT))
    quit(code)

func check(condition: bool, message: String) -> void:
    if not condition:
        failed = true
        push_error(message)

func tasks_for(mask: int) -> Array:
    var tasks: Array = []
    for i in range(6):
        tasks.append({"id": "challenge_16_clear" if i == 5 else "daily_unplayed_song_clear",
            "current": 1 if mask & (1 << i) else 0})
    return tasks

func run_case(label: String, completed: int, pending: int) -> void:
    set_meta("konzetsu_daily_tasks", tasks_for(completed))
    set_meta("konzetsu_pending_clear_mask", pending)
    var scene: Control = load("res://Godot/Scenes/Konzetsu.tscn").instantiate()
    root.add_child(scene)
    var hub: Control = scene.get_node("DesignCanvas/Hub/HubContent")
    var unlocking := (completed & 31) == 31 and (pending & 31) != 0
    check(hub.get_node("HubNode6Locked").visible == ((completed & 31) != 31 or unlocking),
        label + ": initial sixth-door lock state")
    var target := 1
    while not pending & (1 << (target - 1)):
        target += 1
    var prefix := "HubNode%d" % target
    check(hub.get_node(prefix + "Number").visible, label + ": defer clear until entrance finishes")
    var saw_audio := false
    var saw_fade := false
    var saw_inactive_fade := false
    var saw_lock_fade := false
    var audio_triggers := 0
    var previous_streams := {}
    var deadline := Time.get_ticks_msec() + 18000
    while Time.get_ticks_msec() < deadline:
        await process_frame
        for child in scene.get_children():
            if child is AudioStreamPlayer and child.stream != previous_streams.get(child.get_instance_id()):
                previous_streams[child.get_instance_id()] = child.stream
                if child.stream != null and child.stream.resource_path.ends_with("challenge_chain_break.wav"):
                    saw_audio = true
                    audio_triggers += 1
        var active: Control = hub.get_node(prefix + "Badge")
        var inactive: Control = hub.get_node(prefix + "BadgeInactive")
        if active.visible and active.modulate.a > 0.001 and active.modulate.a < 0.999:
            saw_fade = true
            check(not scene.get_node("DesignCanvas/LoadingBg").visible,
                label + ": animation must follow entrance")
        if inactive.visible and inactive.modulate.a > 0.001 and inactive.modulate.a < 0.999:
            saw_inactive_fade = true
            check(not active.visible, label + ": swap images only after active badge fades out")
        var lock: Control = hub.get_node("HubNode6Locked")
        if unlocking and lock.visible and lock.modulate.a > 0.001 and lock.modulate.a < 0.999:
            saw_lock_fade = true
            check(hub.get_node("HubNode6Hit").disabled, label + ": keep sixth door blocked during clear")
        var final_badge: Control = inactive if unlocking or target == 6 else active
        if saw_audio and final_badge.visible and final_badge.modulate.a >= 0.999 and not hub.get_node(prefix + "Number").visible:
            # The final ease curve approaches one before the pending mask clears.
            await create_timer(0.15).timeout
            break
    check(saw_audio and audio_triggers == 1, label + ": exactly one original clear audio trigger")
    check(saw_fade, label + ": active badge must animate")
    if unlocking or target == 6:
        check(saw_inactive_fade, label + ": inactive badge must fade back in")
    if unlocking:
        check(saw_lock_fade, label + ": lock must fade out")
    check(hub.get_node("HubNode6Locked").visible == ((completed & 31) != 31), label + ": final lock state")
    check(hub.get_node("HubNode6Hit").disabled == ((completed & 31) != 31), label + ": final interaction state")
    var chains := ["HubTopChainLeft", "HubTopChainRight", "HubCenterChainLeft",
        "HubCenterChainRight", "HubBottomChainLeft", "HubBottomChainRight"]
    for i in range(6):
        check(is_equal_approx(hub.get_node(chains[i]).modulate.a, 0.0 if completed & (1 << i) else 1.0),
            label + ": central chain %d" % (i + 1))
    var clear_audio := scene.get_node_or_null("KonzetsuClearSFX") as AudioStreamPlayer
    check(clear_audio != null and clear_audio.playing, label + ": clear sound continues past the visual transition")
    scene.open_konzetsu_challenge(1)
    await process_frame
    check(is_instance_valid(clear_audio) and clear_audio.playing, label + ": clicking a door must not cut off clear audio")
    scene.close_konzetsu_challenge()
    scene.queue_free()
    await process_frame
    var reentry: Control = load("res://Godot/Scenes/Konzetsu.tscn").instantiate()
    root.add_child(reentry)
    var reentry_hub := reentry.get_node("DesignCanvas/Hub/HubContent")
    check(not reentry_hub.get_node(prefix + "Number").visible, label + ": no clear replay on reentry")
    check(reentry_hub.get_node("HubNode6Locked").visible == ((completed & 31) != 31), label + ": reentry lock state")
    reentry.queue_free()
    await process_frame
    print("CHECKED: ", label)

func run() -> void:
    await run_case("single door", 1, 1)
    await run_case("multiple doors", 3, 3)
    await run_case("fifth door unlock", 31, 16)
    await run_case("sixth door completion", 63, 32)
    if not failed:
        print("PASS: original clear audio, staged image changes, lock fade, central chains and reentry")
    finish(1 if failed else 0)
