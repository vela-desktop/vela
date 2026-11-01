// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SCENE_SCENE_H
#define VELA_SCENE_SCENE_H

// Vela's scene (docs/renderer.md §5): what is on screen, bottom to top. It
// takes the place of wlr_scene.
//
// - App surfaces are read live (§5.2): a surface node points only at the
//   root surface; subsurfaces, buffers and synchronized state are read from
//   wlroots when drawing.
// - Our own nodes (trees, rectangles, snapshots) hold only what is ours:
//   position, visibility, opacity.
// - Damage isn't computed on every change: on every frame the output
//   compares what it draws with the previous frame (scene/frame.c). Here
//   every change just asks all outputs for a new frame.
//
// Logical coordinates (double): one unit = one pixel at 100% scale (§3.1).
//
// Who owns what: each node is created and destroyed by its owner (a window,
// a panel, an animation), with vela_*_create and vela_node_destroy.
// Destroying a tree doesn't destroy its children: they are left orphaned
// (out of the scene, not drawn) until their owner destroys them too.

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/box.h>

struct vela_scene;
struct vela_tree;
struct wlr_buffer;
struct wlr_compositor;
struct wlr_linux_dmabuf_v1;
struct wlr_surface;
struct wlr_tearing_control_manager_v1;
struct wlr_texture;
struct wlr_xdg_popup;

enum vela_node_type {
    VELA_NODE_TREE,
    VELA_NODE_SURFACE,
    VELA_NODE_RECT,
    VELA_NODE_BUFFER,
};

struct vela_node {
    enum vela_node_type type;
    struct vela_scene *scene;
    struct vela_tree *parent; // NULL: orphan (or the root)
    struct wl_list link; // among the parent's children, bottom to top
    double x, y; // relative to the parent
    bool enabled;
    float opacity; // multiplied by the ancestors'

    // Who owns it (to know what is under the cursor).
    void *data;
    // Rectangles and images usually let clicks through (previews, snapshots);
    // those of the title bar don't.
    bool hittable;
    // Visible but takes no input, with its children (such as the dragged icon,
    // which sits under the cursor and must not cover the drop target).
    bool ignores_input;
    // The children don't inherit an ancestor's shape clip (popups: a menu can
    // go outside the window).
    bool unclipped;
};

// A tree's shape (docs/renderer.md §8): the children clipped to a rounded
// rectangle, with a shadow below. Logical coordinates, relative to the tree.
struct vela_shape {
    bool enabled;
    double x, y, width, height;
    double radius;
    bool shadow;
    bool active; // the active window has a stronger shadow
};

struct vela_tree {
    struct vela_node node;
    struct wl_list children; // vela_node.link, bottom to top
    struct vela_shape shape;
};

// An app surface with its subsurfaces, read live.
struct vela_surface_node {
    struct vela_node node;
    struct wlr_surface *surface;
};

// A solid rectangle (premultiplied sRGB color, as in wlroots).
struct vela_rect_node {
    struct vela_node node;
    double width, height;
    struct wlr_render_color color;
};

// A "frozen" buffer (part of a snapshot, title bar text): it stays valid even
// if the app changes it or quits, because it's locked as long as the node
// exists. The texture must live as long as the buffer.
struct vela_buffer_node {
    struct vela_node node;
    struct wlr_buffer *buffer;
    struct wlr_texture *texture;
    struct wlr_fbox src;
    enum wl_output_transform transform;
    double width, height;
};

// The scene: the root, the outputs drawing it and the choices that apply to
// all of them. Only one, the server's.
struct vela_scene {
    struct vela_tree *root;
    struct wl_list frames; // vela_output_frame.link: the active outputs
    // For per-surface dmabuf feedback (§5.3); may be missing.
    struct wlr_linux_dmabuf_v1 *linux_dmabuf;
    // For the explicit sync release points.
    struct wl_event_loop *event_loop;
    // The acrylic tint of blurs (premultiplied sRGB): Windows 11's dark theme
    // one, or the light one when the shell is light.
    struct wlr_render_color acrylic_tint;
    // The color filters of all outputs: a 3x3 matrix by rows in linear space,
    // if color_filtered. Applied while drawing.
    bool color_filtered;
    float color_filter[9];
    // Night light: how much red, green and blue is left (linear light). On
    // real outputs it goes into the monitor's gamma (outside drawing: no tint
    // in screenshots, direct scanout kept); where that isn't possible, into
    // drawing like the filters. The version changes with the values.
    bool night_active;
    bool night_force_shader; // no gamma LUT: the NVIDIA workaround, or a fade in progress (a11y.c)
    float night_gains[3];
    uint32_t night_version;
    // wp-tearing-control: fullscreen apps that ask to show every frame at
    // once, even mid-screen (games). Granted if allow_tearing (vela.conf
    // "tearing").
    struct wlr_tearing_control_manager_v1 *tearing_control;
    bool allow_tearing;
    // vela.conf "direct-scanout=no": always composite (VELA_SCANOUT=0 wins).
    bool allow_scanout;

    struct wl_listener new_surface;
};

struct vela_scene *vela_scene_create(void);
// The outputs (their frames) and the owners' nodes go first.
void vela_scene_destroy(struct vela_scene *scene);

// To call on every change: asks all outputs for a frame.
void vela_scene_changed(struct vela_scene *scene);

// Surfaces about to reach the screen pass through here: the damage of their
// commits goes to the outputs that show them.
void vela_scene_watch(struct vela_scene *scene, struct wlr_compositor *compositor);

// What is at a point of the layout (only what accepts input).
struct vela_hit {
    struct wlr_surface *surface; // NULL: one of our nodes (the title bar)
    double sx, sy;
    void *owner; // the nearest `data` going up the tree
    struct vela_node *node; // the surface or rectangle hit
};
struct vela_hit vela_scene_at(struct vela_scene *scene, double lx, double ly);

struct vela_tree *vela_tree_create(struct vela_tree *parent);
struct vela_surface_node *vela_surface_node_create(struct vela_tree *parent, struct wlr_surface *surface);
struct vela_rect_node *vela_rect_node_create(struct vela_tree *parent, double width, double height,
    const struct wlr_render_color *color);
// `buffer` stays locked as long as the node exists.
struct vela_buffer_node *vela_buffer_node_create(struct vela_tree *parent, struct wlr_buffer *buffer,
    struct wlr_texture *texture, const struct wlr_fbox *src, enum wl_output_transform transform, double width,
    double height);
void vela_node_destroy(struct vela_node *node);

void vela_node_set_position(struct vela_node *node, double x, double y);
// Absolute position (logical).
void vela_node_coords(const struct vela_node *node, double *lx, double *ly);
void vela_node_set_enabled(struct vela_node *node, bool enabled);
void vela_node_set_opacity(struct vela_node *node, float opacity);
void vela_node_raise_to_top(struct vela_node *node);
void vela_node_place_above(struct vela_node *node, struct vela_node *sibling);
void vela_node_place_below(struct vela_node *node, struct vela_node *sibling);
void vela_node_reparent(struct vela_node *node, struct vela_tree *parent);
// Which is drawn on top: > 0 `node`, < 0 `other`, 0 when one contains the
// other (or they are in different trees).
int vela_node_order(const struct vela_node *node, const struct vela_node *other);

void vela_tree_set_shape(struct vela_tree *tree, const struct vela_shape *shape);
void vela_rect_node_set_size(struct vela_rect_node *rect, double width, double height);
void vela_rect_node_set_color(struct vela_rect_node *rect, const struct wlr_render_color *color);
void vela_buffer_node_set_size(struct vela_buffer_node *buffer, double width, double height);

// Every surface of a surface tree (root and mapped subsurfaces), in drawing
// order, with the position relative to the root.
typedef void (*vela_surface_iterator)(struct wlr_surface *surface, int x, int y, void *data);
void vela_for_each_surface(struct wlr_surface *root, vela_surface_iterator iterator, void *data);

// A popup's position relative to the parent surface's origin (even when the
// parent is a shell panel).
void vela_popup_position(struct wlr_xdg_popup *popup, double *x, double *y);

#endif
