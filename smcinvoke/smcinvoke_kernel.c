// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/anon_inodes.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/elf.h>
#include "smcinvoke.h"
#include "smcinvoke_object.h"
#include "IClientEnv.h"

#if !IS_ENABLED(CONFIG_QSEECOM)
#include "linux/qseecom.h"
#include "misc/qseecom_kernel.h"
#include "IQSEEComCompat.h"
#include "IQSEEComCompatAppLoader.h"
#endif

const uint32_t CQSEEComCompatAppLoader_UID = 122;

struct qseecom_compat_context {
	void *dev; /* in/out */
	unsigned char *sbuf; /* in/out */
	uint32_t sbuf_len; /* in/out */
	struct qtee_shm shm;
	uint8_t app_arch;
	struct Object client_env;
	struct Object app_loader;
	struct Object app_controller;
};

struct tzobject_context {
	int fd;
	struct kref refs;
};

static int invoke_over_smcinvoke(void *cxt,
			uint32_t op,
			union ObjectArg *args,
			uint32_t counts);

static struct Object tzobject_new(int fd)
{
	struct tzobject_context *me =
			kzalloc(sizeof(struct tzobject_context), GFP_KERNEL);
	if (!me)
		return Object_NULL;

	kref_init(&me->refs);
	me->fd = fd;
	pr_debug("%s: me->fd = %d, me->refs = %u\n", __func__,
			me->fd, kref_read(&me->refs));
	return (struct Object) { invoke_over_smcinvoke, me };
}

static void tzobject_delete(struct kref *refs)
{
	struct tzobject_context *me = container_of(refs,
				struct tzobject_context, refs);

	pr_info("%s: me->fd = %d, me->refs = %d, files = %p\n",
		__func__, me->fd, kref_read(&me->refs), current->files);
	/*
	 * after _close_fd(), ref_cnt will be 0,
	 * but smcinvoke_release() was still not called,
	 * so we first call smcinvoke_release_from_kernel_client() to
	 * free filp and ask TZ to release object, then call _close_fd()
	 */
	smcinvoke_release_from_kernel_client(me->fd);
	close_fd(me->fd);
	kfree(me);
}

int getObjectFromHandle(int handle, struct Object *obj)
{
	int ret = 0;

	if (handle == SMCINVOKE_USERSPACE_OBJ_NULL) {
		/* NULL object*/
		Object_ASSIGN_NULL(*obj);
	} else if (handle > SMCINVOKE_USERSPACE_OBJ_NULL) {
		*obj = tzobject_new(handle);
		if (Object_isNull(*obj))
			ret = OBJECT_ERROR_BADOBJ;
	} else {
		pr_err("CBobj not supported for handle %d\n", handle);
		ret = OBJECT_ERROR_BADOBJ;
	}

	return ret;
}

int getHandleFromObject(struct Object obj, int *handle)
{
	int ret = 0;

	if (Object_isNull(obj)) {
	/* set NULL Object's fd to be -1 */
		*handle = SMCINVOKE_USERSPACE_OBJ_NULL;
		return ret;
	}

	if (obj.invoke == invoke_over_smcinvoke) {
		struct tzobject_context *ctx = (struct tzobject_context *)(obj.context);

		if (ctx != NULL) {
			*handle = ctx->fd;
		} else {
			pr_err("Failed to get tzobject_context obj handle, ret = %d\n", ret);
			ret = OBJECT_ERROR_BADOBJ;
		}
	} else {
		pr_err("CBobj not supported\n");
		ret = OBJECT_ERROR_BADOBJ;
	}

	return ret;
}

static int marshalIn(struct smcinvoke_cmd_req *req,
			union smcinvoke_arg *argptr,
			uint32_t op, union ObjectArg *args,
			uint32_t counts)
{
	size_t i = 0;

	req->op = op;
	req->counts = counts;
	req->argsize = sizeof(union smcinvoke_arg);
	req->args = (uintptr_t)argptr;

	FOR_ARGS(i, counts, buffers) {
		argptr[i].b.addr = (uintptr_t) args[i].b.ptr;
		argptr[i].b.size = args[i].b.size;
	}

	FOR_ARGS(i, counts, OI) {
		int handle = -1, ret;

		ret = getHandleFromObject(args[i].o, &handle);
		if (ret) {
			pr_err("invalid OI[%zu]\n", i);
			return OBJECT_ERROR_BADOBJ;
		}
		argptr[i].o.fd = handle;
	}

	FOR_ARGS(i, counts, OO) {
		argptr[i].o.fd = SMCINVOKE_USERSPACE_OBJ_NULL;
	}
	return OBJECT_OK;
}

static int marshalOut(struct smcinvoke_cmd_req *req,
			union smcinvoke_arg *argptr,
			union ObjectArg *args, uint32_t counts,
			struct tzobject_context *me)
{
	int ret = req->result;
	bool failed = false;
	size_t i = 0;

	argptr = (union smcinvoke_arg *)(uintptr_t)(req->args);

	FOR_ARGS(i, counts, BO) {
		args[i].b.size = argptr[i].b.size;
	}

	FOR_ARGS(i, counts, OO) {
		ret = getObjectFromHandle(argptr[i].o.fd, &(args[i].o));
		if (ret) {
			pr_err("Failed to get OO[%zu] from handle = %d\n",
				i, (int)argptr[i].o.fd);
			failed = true;
			break;
		}
		pr_debug("Succeed to create OO for args[%zu].o, fd = %d\n",
			i, (int)argptr[i].o.fd);
	}
	if (failed) {
		FOR_ARGS(i, counts, OO) {
			Object_ASSIGN_NULL(args[i].o);
		}
		/* Only overwrite ret value if invoke result is 0 */
		if (ret == 0)
			ret = OBJECT_ERROR_BADOBJ;
	}
	return ret;
}

static int invoke_over_smcinvoke(void *cxt,
			uint32_t op,
			union ObjectArg *args,
			uint32_t counts)
{
	int ret = OBJECT_OK;
	struct smcinvoke_cmd_req req = {0, 0, 0, 0, 0};
	size_t i = 0;
	struct tzobject_context *me = NULL;
	uint32_t method;
	union smcinvoke_arg *argptr = NULL;

	FOR_ARGS(i, counts, OO) {
		args[i].o = Object_NULL;
	}

	me = (struct tzobject_context *)cxt;
	method = ObjectOp_methodID(op);
	pr_debug("%s: cxt = %p, fd = %d, op = %u, cnt = %x, refs = %u\n",
			__func__, me, me->fd, op, counts, kref_read(&me->refs));

	if (ObjectOp_isLocal(op)) {
		switch (method) {
		case Object_OP_retain:
			kref_get(&me->refs);
			return OBJECT_OK;
		case Object_OP_release:
			kref_put(&me->refs, tzobject_delete);
			return OBJECT_OK;
		}
		return OBJECT_ERROR_REMOTE;
	}

	argptr = kcalloc(OBJECT_COUNTS_TOTAL(counts),
			sizeof(union smcinvoke_arg), GFP_KERNEL);
	if (argptr == NULL)
		return OBJECT_ERROR_KMEM;

	ret = marshalIn(&req, argptr, op, args, counts);
	if (ret)
		goto exit;

	ret = process_invoke_request_from_kernel_client(me->fd, &req);
	if (ret) {
		pr_err("INVOKE failed with ret = %d, result = %d\n"
			"obj.context = %p, fd = %d, op = %d, counts = 0x%x\n",
			ret, req.result, me, me->fd, op, counts);
		FOR_ARGS(i, counts, OO) {
			struct smcinvoke_obj obj = argptr[i].o;

			if (obj.fd >= 0) {
				pr_err("Close OO[%zu].fd = %d\n", i, obj.fd);
				close_fd(obj.fd);
			}
		}
		ret = OBJECT_ERROR_KMEM;
		goto exit;
	}

	if (!req.result)
		ret = marshalOut(&req, argptr, args, counts, me);
exit:
	kfree(argptr);
	return ret | req.result;
}

static int get_root_obj(struct Object *rootObj)
{
	int ret = 0;
	int root_fd = -1;

	ret = get_root_fd(&root_fd);
	if (ret) {
		pr_err("Failed to get root fd, ret = %d\n");
		return ret;
	}
	*rootObj = tzobject_new(root_fd);
	if (Object_isNull(*rootObj)) {
		close_fd(root_fd);
		ret = -ENOMEM;
	}
	return ret;
}

/*
 * Get a client environment using CBOR encoded credentials
 * with UID of SYSTEM_UID (1000)
 */
int32_t get_client_env_object(struct Object *clientEnvObj)
{
	int32_t  ret = OBJECT_ERROR;
	struct Object rootObj = Object_NULL;
	/* Hardcode self cred buffer in CBOR encoded format.
	 * CBOR encoded credentials is created using following parameters,
	 * #define ATTR_UID        1
	 * #define ATTR_PKG_NAME   3
	 * #define SYSTEM_UID      1000
	 * static const uint8_t bufString[] = {"UefiSmcInvoke"};
	 */
	uint8_t encodedBuf[] = {0xA2, 0x01, 0x19, 0x03, 0xE8, 0x03, 0x6E, 0x55,
				0x65, 0x66, 0x69, 0x53, 0x6D, 0x63, 0x49, 0x6E,
				0x76, 0x6F, 0x6B, 0x65, 0x0};

	/* get rootObj */
	ret = get_root_obj(&rootObj);
	if (ret) {
		pr_err("Failed to create rootobj\n");
		return ret;
	}

	/* get client env */
	ret = IClientEnv_registerLegacy(rootObj, encodedBuf,
			sizeof(encodedBuf), clientEnvObj);
	if (ret)
		pr_err("Failed to get ClientEnvObject, ret = %d\n", ret);
	Object_release(rootObj);
	return ret;
}
EXPORT_SYMBOL(get_client_env_object);

#if !IS_ENABLED(CONFIG_QSEECOM)

static int load_app(struct qseecom_compat_context *cxt, const char *app_name)
{
	size_t fw_size = 0;
	u8 *imgbuf_va = NULL;
	int ret = 0;
	char dist_name[MAX_APP_NAME_SIZE] = {0};
	size_t dist_name_len = 0;
	struct qtee_shm shm = {0};

	if (strnlen(app_name, MAX_APP_NAME_SIZE) == MAX_APP_NAME_SIZE) {
		pr_err("The app_name (%s) with length %zu is not valid\n",
			app_name, strnlen(app_name, MAX_APP_NAME_SIZE));
		return -EINVAL;
	}

	ret = IQSEEComCompatAppLoader_lookupTA(cxt->app_loader,
		app_name, strlen(app_name), &cxt->app_controller);
	if (!ret) {
		pr_info("app %s exists\n", app_name);
		return ret;
	}

	imgbuf_va = firmware_request_from_smcinvoke(app_name, &fw_size, &shm);
	if (imgbuf_va == NULL) {
		pr_err("Failed on firmware_request_from_smcinvoke\n");
		return -EINVAL;
	}

	ret = IQSEEComCompatAppLoader_loadFromBuffer(
			cxt->app_loader, imgbuf_va, fw_size,
			app_name, strlen(app_name),
			dist_name, MAX_APP_NAME_SIZE, &dist_name_len,
			&cxt->app_controller);
	if (ret) {
		pr_err("loadFromBuffer failed for app %s, ret = %d\n",
				app_name, ret);
		goto exit_release_shm;
	}
	cxt->app_arch = *(uint8_t *)(imgbuf_va + EI_CLASS);

	pr_info("%s %d, loaded app %s, dist_name %s, dist_name_len %zu\n",
		__func__, __LINE__, app_name, dist_name, dist_name_len);

exit_release_shm:
	qtee_shmbridge_free_shm(&shm);
	return ret;
}

int qseecom_start_app(struct qseecom_handle **handle,
					char *app_name, uint32_t size)
{
	int ret = 0;
	struct qseecom_compat_context *cxt = NULL;

	pr_warn("%s, start app %s, size %zu\n",
		__func__, app_name, size);
	if (app_name == NULL || handle == NULL) {
		pr_err("app_name is null or invalid handle\n");
		return -EINVAL;
	}
	/* allocate qseecom_compat_context */
	cxt = kzalloc(sizeof(struct qseecom_compat_context), GFP_KERNEL);
	if (!cxt)
		return -ENOMEM;

	/* get client env */
	ret = get_client_env_object(&cxt->client_env);
	if (ret) {
		pr_err("failed to get clientEnv when loading app %s, ret %d\n",
			app_name, ret);
		ret = -EINVAL;
		goto exit_free_cxt;
	}
	/* get apploader with CQSEEComCompatAppLoader_UID */
	ret = IClientEnv_open(cxt->client_env, CQSEEComCompatAppLoader_UID,
				&cxt->app_loader);
	if (ret) {
		pr_err("failed to get apploader when loading app %s, ret %d\n",
			app_name, ret);
		ret = -EINVAL;
		goto exit_release_clientenv;
	}

	/* load app*/
	ret = load_app(cxt, app_name);
	if (ret) {
		pr_err("failed to load app %s, ret = %d\n",
			app_name, ret);
		ret = -EINVAL;
		goto exit_release_apploader;
	}

	/* Get the physical address of the req/resp buffer */
	ret = qtee_shmbridge_allocate_shm(size, &cxt->shm);

	if (ret) {
		pr_err("qtee_shmbridge_allocate_shm failed, ret :%d\n", ret);
		ret = -EINVAL;
		goto exit_release_appcontroller;
	}
	cxt->sbuf = cxt->shm.vaddr;
	cxt->sbuf_len = size;
	*handle = (struct qseecom_handle *)cxt;

	return ret;

exit_release_appcontroller:
	Object_release(cxt->app_controller);
exit_release_apploader:
	Object_release(cxt->app_loader);
exit_release_clientenv:
	Object_release(cxt->client_env);
exit_free_cxt:
	kfree(cxt);

	return ret;
}
EXPORT_SYMBOL(qseecom_start_app);

int qseecom_shutdown_app(struct qseecom_handle **handle)
{
	struct qseecom_compat_context *cxt =
		(struct qseecom_compat_context *)(*handle);

	if ((handle == NULL)  || (*handle == NULL)) {
		pr_err("Handle is NULL\n");
		return -EINVAL;
	}

	qtee_shmbridge_free_shm(&cxt->shm);
	Object_release(cxt->app_controller);
	Object_release(cxt->app_loader);
	Object_release(cxt->client_env);
	kfree(cxt);
	*handle = NULL;
	return 0;
}
EXPORT_SYMBOL(qseecom_shutdown_app);

int qseecom_send_command(struct qseecom_handle *handle, void *send_buf,
			uint32_t sbuf_len, void *resp_buf, uint32_t rbuf_len)
{
	struct qseecom_compat_context *cxt =
			(struct qseecom_compat_context *)handle;
	size_t out_len = 0;

	pr_debug("%s, sbuf_len %u, rbuf_len %u\n",
		__func__, sbuf_len, rbuf_len);

	if (!handle || !send_buf || !resp_buf || !sbuf_len || !rbuf_len) {
		pr_err("One of params is invalid. %s, handle %x, send_buf %x,resp_buf %x,sbuf_len %u, rbuf_len %u\n",
			 __func__, handle, send_buf, resp_buf, sbuf_len, rbuf_len);
		return -EINVAL;
	}
	return IQSEEComCompat_sendRequest(cxt->app_controller,
				  send_buf, sbuf_len,
				  resp_buf, rbuf_len,
				  send_buf, sbuf_len, &out_len,
				  resp_buf, rbuf_len, &out_len,
				  NULL, 0, /* embedded offset array */
				  (cxt->app_arch == ELFCLASS64),
				  Object_NULL, Object_NULL,
				  Object_NULL, Object_NULL);
}
EXPORT_SYMBOL(qseecom_send_command);
#endif
