// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/scene.h"

#include "listen.h"
#include "output.h"
#include "scene/frame.h"
#include "scene/surface.h"

#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>

// ----------------------------------------------------------------- nodes --

static void node_init(struct vela_node *node, enum vela_node_type type, struct vela_scene *scene,
    struct vela_tree *parent)
{
    node->type = type;
    node->scene = scene;
    node->enabled = true;
    node->opacity = 1.0f;
    wl_list_init(&node->link);
    if (parent) {
        node->parent = parent;
        wl_list_insert(parent->children.prev, &node->link);
    }
    vela_scene_changed(scene);
}

static void detach(struct vela_node *node)
{
    if (node->parent) {
        wl_list_remove(&node->link);
        wl_list_init(&node->link);
        node->parent = NULL;
    }
}

static struct vela_tree *tree_new(struct vela_scene *scene, struct vela_tree *parent)
{
    struct vela_tree *tree = calloc(1, sizeof(*tree));
    wl_list_init(&tree->children);
    node_init(&tree->node, VELA_NODE_TREE, scene, parent);
    return tree;
}

struct vela_tree *vela_tree_create(struct vela_tree *parent)
{
    return tree_new(parent->node.scene, parent);
}

struct vela_surface_node *vela_surface_node_create(struct vela_tree *parent, struct wlr_surface *surface)
{
    struct vela_surface_node *node = calloc(1, sizeof(*node));
    node->surface = surface;
    node_init(&node->node, VELA_NODE_SURFACE, parent->node.scene, parent);
    return node;
}

struct vela_rect_node *vela_rect_node_create(struct vela_tree *parent, double width, double height,
    const struct wlr_render_color *color)
{
    struct vela_rect_node *rect = calloc(1, sizeof(*rect));
    rect->width = width;
    rect->height = height;
    rect->color = *color;
    node_init(&rect->node, VELA_NODE_RECT, parent->node.scene, parent);
    return rect;
}

struct vela_buffer_node *vela_buffer_node_create(struct vela_tree *parent, struct wlr_buffer *buffer,
    struct wlr_texture *texture, const struct wlr_fbox *src, enum wl_output_transform transform, double width,
    double height)
{
    struct vela_buffer_node *node = calloc(1, sizeof(*node));
    node->buffer = wlr_buffer_lock(buffer);
    node->texture = texture;
    node->src = *src;
    node->transform = transform;
    node->width = width;
    node->height = height;
    node_init(&node->node, VELA_NODE_BUFFER, parent->node.scene, parent);
    return node;
}

void vela_node_destroy(struct vela_node *node)
{
    if (!node) {
        return;
    }
    struct vela_scene *scene = node->scene;
    detach(node);
    if (node->type == VELA_NODE_TREE) {
        // The children are left orphaned: their owner will destroy them.
        struct vela_tree *tree = (struct vela_tree *)node;
        struct vela_node *child, *tmp;
        wl_list_for_each_safe (child, tmp, &tree->children, link) {
            wl_list_remove(&child->link);
            wl_list_init(&child->link);
            child->parent = NULL;
        }
    } else if (node->type == VELA_NODE_BUFFER) {
        wlr_buffer_unlock(((struct vela_buffer_node *)node)->buffer);
    }
    free(node);
    vela_scene_changed(scene);
}

void vela_node_set_position(struct vela_node *node, double x, double y)
{
    if (x == node->x && y == node->y) {
        return;
    }
    node->x = x;
    node->y = y;
    vela_scene_changed(node->scene);
}

void vela_node_coords(const struct vela_node *node, double *lx, double *ly)
{
    *lx = 0.0;
    *ly = 0.0;
    for (; node; node = node->parent ? &node->parent->node : NULL) {
        *lx += node->x;
        *ly += node->y;
    }
}

void vela_node_set_enabled(struct vela_node *node, bool enabled)
{
    if (enabled == node->enabled) {
        return;
    }
    node->enabled = enabled;
    vela_scene_changed(node->scene);
}


void vela_node_set_opacity(struct vela_node *node, float opacity)
{
    if (opacity == node->opacity) {
        return;
    }
    node->opacity = opacity;
    vela_scene_changed(node->scene);
}

void vela_node_raise_to_top(struct vela_node *node)
{
    struct vela_tree *parent = node->parent;
    if (!parent || parent->children.prev == &node->link) {
        return;
    }
    wl_list_remove(&node->link);
    wl_list_insert(parent->children.prev, &node->link);
    vela_scene_changed(node->scene);
}

void vela_node_place_above(struct vela_node *node, struct vela_node *sibling)
{
    if (sibling == node || !node->parent || sibling->parent != node->parent) {
        return;
    }
    wl_list_remove(&node->link);
    wl_list_insert(&sibling->link, &node->link);
    vela_scene_changed(node->scene);
}

void vela_node_place_below(struct vela_node *node, struct vela_node *sibling)
{
    if (sibling == node || !node->parent || sibling->parent != node->parent) {
        return;
    }
    wl_list_remove(&node->link);
    wl_list_insert(sibling->link.prev, &node->link);
    vela_scene_changed(node->scene);
}

void vela_node_reparent(struct vela_node *node, struct vela_tree *parent)
{
    if (parent == node->parent) {
        return;
    }
    detach(node);
    if (parent) {
        node->parent = parent;
        wl_list_insert(parent->children.prev, &node->link);
    }
    vela_scene_changed(node->scene);
}

static int depth(const struct vela_node *node)
{
    int depth = 0;
    for (; node->parent; node = &node->parent->node) {
        ++depth;
    }
    return depth;
}

int vela_node_order(const struct vela_node *node, const struct vela_node *other)
{
    int a = depth(node);
    int b = depth(other);
    for (; a > b; --a) {
        node = &node->parent->node;
    }
    for (; b > a; --b) {
        other = &other->parent->node;
    }
    if (node == other) {
        return 0; // one inside the other
    }
    // Up to the children of the closest common ancestor: their order decides.
    while (node->parent != other->parent) {
        node = &node->parent->node;
        other = &other->parent->node;
    }
    if (!node->parent) {
        return 0; // different trees
    }
    // Children are bottom to top: `node` is above if it follows `other`.
    for (const struct wl_list *link = other->link.next; link != &other->parent->children; link = link->next) {
        if (link == &node->link) {
            return 1;
        }
    }
    return -1;
}

static bool same_shape(const struct vela_shape *a, const struct vela_shape *b)
{
    return a->enabled == b->enabled && a->x == b->x && a->y == b->y && a->width == b->width
        && a->height == b->height && a->radius == b->radius && a->shadow == b->shadow && a->active == b->active;
}

void vela_tree_set_shape(struct vela_tree *tree, const struct vela_shape *shape)
{
    if (same_shape(&tree->shape, shape)) {
        return;
    }
    tree->shape = *shape;
    vela_scene_changed(tree->node.scene);
}

void vela_rect_node_set_size(struct vela_rect_node *rect, double width, double height)
{
    if (width == rect->width && height == rect->height) {
        return;
    }
    rect->width = width;
    rect->height = height;
    vela_scene_changed(rect->node.scene);
}

void vela_rect_node_set_color(struct vela_rect_node *rect, const struct wlr_render_color *color)
{
    // Like every setter: the same value again is no change. An animation
    // that keeps setting its last color (the snap preview while it rests)
    // would otherwise plan a frame at every vblank.
    if (color->r == rect->color.r && color->g == rect->color.g && color->b == rect->color.b
        && color->a == rect->color.a) {
        return;
    }
    rect->color = *color;
    vela_scene_changed(rect->node.scene);
}

void vela_buffer_node_set_size(struct vela_buffer_node *buffer, double width, double height)
{
    if (width == buffer->width && height == buffer->height) {
        return;
    }
    buffer->width = width;
    buffer->height = height;
    vela_scene_changed(buffer->node.scene);
}

// -------------------------------------------------------------- surfaces --

static void for_each_surface_at(struct wlr_surface *surface, int x, int y, vela_surface_iterator iterator,
    void *data)
{
    struct wlr_subsurface *subsurface;
    wl_list_for_each (subsurface, &surface->current.subsurfaces_below, current.link) {
        if (subsurface->surface->mapped) {
            for_each_surface_at(subsurface->surface, x + subsurface->current.x, y + subsurface->current.y, iterator,
                data);
        }
    }
    iterator(surface, x, y, data);
    wl_list_for_each (subsurface, &surface->current.subsurfaces_above, current.link) {
        if (subsurface->surface->mapped) {
            for_each_surface_at(subsurface->surface, x + subsurface->current.x, y + subsurface->current.y, iterator,
                data);
        }
    }
}

void vela_for_each_surface(struct wlr_surface *root, vela_surface_iterator iterator, void *data)
{
    for_each_surface_at(root, 0, 0, iterator, data);
}

void vela_popup_position(struct wlr_xdg_popup *popup, double *x, double *y)
{
    *x = popup->current.geometry.x - popup->base->geometry.x;
    *y = popup->current.geometry.y - popup->base->geometry.y;
    // Relative to the parent's window (geometry), which can have a shadow
    // margin; a shell panel has none.
    struct wlr_xdg_surface *parent = popup->parent ? wlr_xdg_surface_try_from_wlr_surface(popup->parent) : NULL;
    if (parent) {
        *x += parent->geometry.x;
        *y += parent->geometry.y;
    }
}

// ------------------------------------------------------------------ scene --

static void handle_new_surface(struct wl_listener *listener, void *data)
{
    struct vela_scene *scene = wl_container_of(listener, scene, new_surface);
    vela_surface_state_create(scene, data);
}

struct vela_scene *vela_scene_create(void)
{
    struct vela_scene *scene = calloc(1, sizeof(*scene));
    wl_list_init(&scene->frames);
    wl_list_init(&scene->new_surface.link);
    scene->acrylic_tint = (struct wlr_render_color) { 0.11f * 0.55f, 0.11f * 0.55f, 0.12f * 0.55f, 0.55f };
    scene->night_gains[0] = scene->night_gains[1] = scene->night_gains[2] = 1.0f;
    scene->allow_tearing = true;
    scene->allow_scanout = true;
    scene->root = tree_new(scene, NULL);
    return scene;
}

void vela_scene_destroy(struct vela_scene *scene)
{
    if (scene->rearranged_idle) {
        wl_event_source_remove(scene->rearranged_idle);
    }
    wl_list_remove(&scene->new_surface.link);
    vela_node_destroy(&scene->root->node);
    free(scene);
}

static void rearranged(void *data)
{
    struct vela_scene *scene = data;
    scene->rearranged_idle = NULL;
    if (scene->rearranged) {
        scene->rearranged(scene->rearranged_data);
    }
}

void vela_scene_changed(struct vela_scene *scene)
{
    struct vela_output_frame *frame;
    wl_list_for_each (frame, &scene->frames, link) {
        vela_output_schedule_frame(frame->owner);
    }
    if (scene->rearranged && !scene->rearranged_idle && scene->event_loop) {
        scene->rearranged_idle = wl_event_loop_add_idle(scene->event_loop, rearranged, scene);
    }
}

void vela_scene_watch(struct vela_scene *scene, struct wlr_compositor *compositor)
{
    vela_listen(&compositor->events.new_surface, &scene->new_surface, handle_new_surface);
}

static bool hit_node(struct vela_node *node, double lx, double ly, struct vela_hit *hit)
{
    if (!node->enabled || node->ignores_input) {
        return false;
    }
    double nx = lx - node->x;
    double ny = ly - node->y;
    bool found = false;
    switch (node->type) {
    case VELA_NODE_TREE: {
        struct vela_tree *tree = (struct vela_tree *)node;
        struct vela_node *child;
        wl_list_for_each_reverse (child, &tree->children, link) {
            if (hit_node(child, nx, ny, hit)) {
                found = true;
                break;
            }
        }
        break;
    }
    case VELA_NODE_SURFACE: {
        struct wlr_surface *surface = ((struct vela_surface_node *)node)->surface;
        if (surface->mapped) {
            double density = vela_surface_density(surface);
            struct wlr_surface *sub
                = wlr_surface_surface_at(surface, nx * density, ny * density, &hit->sx, &hit->sy);
            if (sub) {
                hit->surface = sub;
                found = true;
            }
        }
        break;
    }
    case VELA_NODE_RECT:
    case VELA_NODE_BUFFER: {
        // Previews and snapshots let clicks through; the title bar doesn't (no
        // surface: it belongs to the compositor).
        if (!node->hittable) {
            break;
        }
        double width = node->type == VELA_NODE_RECT ? ((struct vela_rect_node *)node)->width
                                                    : ((struct vela_buffer_node *)node)->width;
        double height = node->type == VELA_NODE_RECT ? ((struct vela_rect_node *)node)->height
                                                     : ((struct vela_buffer_node *)node)->height;
        found = nx >= 0 && ny >= 0 && nx < width && ny < height;
        if (found) {
            hit->surface = NULL;
        }
        break;
    }
    }
    if (found && !hit->node) {
        hit->node = node; // the first found is the deepest
    }
    if (found && !hit->owner && node->data) {
        hit->owner = node->data;
    }
    return found;
}

struct vela_hit vela_scene_at(struct vela_scene *scene, double lx, double ly)
{
    struct vela_hit hit = { 0 };
    hit_node(&scene->root->node, lx, ly, &hit);
    return hit;
}
