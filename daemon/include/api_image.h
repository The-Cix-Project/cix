#ifndef API_IMAGE_H
#define API_IMAGE_H

#include <stddef.h>

/*
 * api_image -- REST handlers for images, their manifests, and image
 * recipes (ADR-0107/0108).
 *
 * An image version is a hash of its package manifest rather than of its
 * content (ADR-0155), which is why the manifest endpoints sit here
 * beside the images themselves: changing what an image declares is what
 * produces a new version of it. image.c owns the trees and the
 * versioning; this is where they meet HTTP. See ADR-0249.
 */

void handle_image_create(int fd, const char *body, size_t body_len);
void handle_image_delete(int fd, const char *name);
void handle_image_get_one(int fd, const char *name);
void handle_image_list(int fd);
void handle_image_manifest_set(int fd, const char *name, const char *body, size_t body_len);
void handle_image_manifest_unset(int fd, const char *name, const char *package);
void handle_image_recipe_add(int fd, const char *body, size_t body_len);
void handle_image_recipe_apply(int fd, const char *name);
void handle_image_recipe_delete(int fd, const char *name);
void handle_image_recipe_get(int fd, const char *name);
void handle_image_recipe_list(int fd);
void handle_images_gc(int fd, const char *body, size_t body_len);
void handle_software_list(int fd);

#endif /* API_IMAGE_H */
