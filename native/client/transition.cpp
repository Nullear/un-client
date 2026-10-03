#include "client.hpp"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using namespace godot;

ClientTransition *ClientTransition::instance = nullptr;

void ClientTransition::_bind_methods() {
    ClassDB::bind_method(D_METHOD("switch_to", "scene", "type"), &ClientTransition::switch_to, DEFVAL(0));
}

ClientTransition *ClientTransition::get_or_create(SceneTree *tree) {
    if (instance) return instance;
    ClientTransition *overlay = memnew(ClientTransition);
    overlay->set_name("GlobalSceneTransition");
    instance = overlay;
    tree->get_root()->call_deferred("add_child", overlay);
    return overlay;
}

void ClientTransition::_ready() {
    instance = this;
    set_layer(100);
    mask = memnew(TextureRect);
    mask->set_name("TriangleMask");
    mask->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    mask->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    mask->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    mask->set_stretch_mode(TextureRect::STRETCH_SCALE);
    Ref<Texture2D> background = ResourceLoader::get_singleton()->load(
        "res://assets/resources/img/transition/Transition-BGAsset.png");
    mask->set_texture(background);
    Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://native/client/transition.gdshader");
    material.instantiate();
    material->set_shader(shader);
    material->set_shader_parameter("background", background);
    mask->set_material(material);
    add_child(mask);
    mask->set_visible(false);
    sfx = memnew(AudioStreamPlayer);
    sfx->set_name("TransitionSFX");
    add_child(sfx);
}

void ClientTransition::switch_to(const String &scene, int type) {
    if (state != 0 || !ResourceLoader::get_singleton()->exists(scene)) return;
    destination = scene;
    scene_preparation_attempted = false;
    transition_type = std::clamp(type, 0, 2);
    elapsed = 0;
    state = 1;
    mask->set_visible(true);
    material->set_shader_parameter("transition_type", transition_type);
    material->set_shader_parameter("revealing", false);
    material->set_shader_parameter("progress", 0.0f);
    sfx->set_stream(ResourceLoader::get_singleton()->load(
        "res://assets/resources/audio/sfx/transition/70_Transition_close_draft1.ogg"));
    sfx->play();
}

void ClientTransition::_process(double delta) {
    if (state == 0) return;
    mask->set_position(Vector2(0, 0));
    mask->set_size(get_viewport()->get_visible_rect().size);
    const Vector2 size = mask->get_size();
    material->set_shader_parameter("aspect", size.y > 0 ? size.x / size.y : 1920.0 / 1080.0);
    if (state == 1 || state == 3) {
        elapsed = std::min(0.85, elapsed + delta);
        const float t = float(elapsed / 0.85);
        material->set_shader_parameter("progress", t);
        if (state == 1 && !scene_preparation_attempted && elapsed >= 0.85) {
            scene_preparation_attempted = true;
            Ref<PackedScene> packed = ResourceLoader::get_singleton()->load(destination);
            if (packed.is_valid()) {
                Node *prepared = packed->instantiate();
                if (Control *control = Object::cast_to<Control>(prepared)) {
                    control->set_visible(false);
                    get_tree()->get_root()->add_child(prepared);
                    control->set_process(false);
                    prepared_scene = prepared;
                } else if (prepared) {
                    memdelete(prepared);
                }
            }
        }
        if (elapsed < 0.85) return;
        if (state == 3) {
            state = 0;
            mask->set_visible(false);
            return;
        }
        state = 2;
        hold = 0;
        Error error = OK;
        if (prepared_scene) {
            Node *old_scene = get_tree()->get_current_scene();
            Node *next_scene = prepared_scene;
            prepared_scene = nullptr;
            if (old_scene) old_scene->queue_free();
            get_tree()->set_current_scene(next_scene);
            Control *control = Object::cast_to<Control>(next_scene);
            control->set_visible(true);
            control->set_process(true);
        } else {
            error = get_tree()->change_scene_to_file(destination);
        }
        if (error != OK) {
            UtilityFunctions::push_error("Failed to change scene: ", destination);
            state = 3;
            elapsed = 0;
            material->set_shader_parameter("revealing", true);
            return;
        }
    } else if (state == 2) {
        hold += delta;
        Node *current = get_tree()->get_current_scene();
        if (hold < 0.1 || !current || current->get_scene_file_path() != destination) return;
        if (ClientScreen *screen = Object::cast_to<ClientScreen>(current))
            if (!screen->is_scene_prepared()) return;
        state = 3;
        elapsed = 0;
        material->set_shader_parameter("revealing", true);
        material->set_shader_parameter("progress", 0.0f);
        sfx->set_stream(ResourceLoader::get_singleton()->load(
            "res://assets/resources/audio/sfx/transition/70.5_Transition_open_draft1.ogg"));
        sfx->play();
    }
}

void ClientTransition::_exit_tree() {
    if (prepared_scene) prepared_scene->queue_free();
    if (instance == this) instance = nullptr;
}
