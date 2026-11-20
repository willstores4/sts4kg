#include <stddef.h>
#include <sys/mman.h>
#include "ps4-libjbc/jailbreak.h"
#include "struct.h"

asm("clear_stack:\nmov $0x800,%ecx\nxor %rax, %rax\n.L1:\npush %rax\nloop .L1\nadd $0x4000,%rsp\nret");
void clear_stack(void);

static void int32ToHex(int32_t Value, char* Hex) {
	Hex[0] = '0';
	Hex[1] = 'x';
	for (int i = 7, x = 2; i >= 0; i--, x++) {
		int BytePart = (Value >> (4 * i)) & 0x0F;
		Hex[x] = (BytePart < 10) ? ('0' + BytePart) : ('A' + (BytePart - 10));
	}
	Hex[10] = 0;
}

static void concat(const char* StringA, const char* StringB, char* Output) {
	int Offset = 0;
	for (int i = 0; StringA[i] != 0; i++) Output[Offset++] = StringA[i];
	for (int i = 0; StringB[i] != 0; i++) Output[Offset++] = StringB[i];
	Output[Offset] = 0;
}

static int url_has_json_ext(const char* url) {
	if (!url) return 0;
	int end = 0;
	while (url[end] != 0 && url[end] != '?' && url[end] != '#') end++;
	if (end < 5) return 0;
	char a = url[end - 5];
	char b = (url[end - 4] >= 'A' && url[end - 4] <= 'Z') ? (char)(url[end - 4] + 32) : url[end - 4];
	char c = (url[end - 3] >= 'A' && url[end - 3] <= 'Z') ? (char)(url[end - 3] + 32) : url[end - 3];
	char d = (url[end - 2] >= 'A' && url[end - 2] <= 'Z') ? (char)(url[end - 2] + 32) : url[end - 2];
	char e = (url[end - 1] >= 'A' && url[end - 1] <= 'Z') ? (char)(url[end - 1] + 32) : url[end - 1];
	return a == '.' && b == 'j' && c == 's' && d == 'o' && e == 'n';
}

int main(void) {
	struct jbc_cred cred;
	jbc_get_cred(&cred);
	jbc_jailbreak_cred(&cred);

	cred.jdir = 0;
	cred.sceProcType = 0x3800000000000010ULL;
	cred.sonyCred = 0x40001c0000000000ULL;
	cred.sceProcCap = 0x900000000000ff00ULL;
	jbc_set_cred(&cred);

	clear_stack();

	void* libSceSysUtil = dlopen("/system/common/lib/libSceSysUtil.sprx", 0);
	int(*sceSysUtilSendSystemNotificationWithText)(int, const char*) =
		libSceSysUtil ? dlsym(libSceSysUtil, "sceSysUtilSendSystemNotificationWithText") : NULL;

	void* usrsrv = dlopen("/system/common/lib/libSceUserService.sprx", 0);
	int(*sceUserServiceInitialize)(OrbisUserServiceInitializeParams*) =
		usrsrv ? dlsym(usrsrv, "sceUserServiceInitialize") : NULL;
	int(*sceUserServiceGetForegroundUser)(int*) =
		usrsrv ? dlsym(usrsrv, "sceUserServiceGetForegroundUser") : NULL;
	int(*sceUserServiceGetInitialUser)(int*) =
		usrsrv ? dlsym(usrsrv, "sceUserServiceGetInitialUser") : NULL;
	int(*sceUserServiceTerminate)(void) =
		usrsrv ? dlsym(usrsrv, "sceUserServiceTerminate") : NULL;

	void* bgft = dlopen("/system/common/lib/libSceBgft.sprx", 0);
	int(*sceBgftInitialize)(struct bgft_init_params*) =
		bgft ? dlsym(bgft, "sceBgftServiceIntInit") : NULL;
	if (bgft && !sceBgftInitialize) sceBgftInitialize = dlsym(bgft, "sceBgftServiceInit");
	int(*sceBgftDownloadRegisterTask)(struct bgft_download_param*, int*) =
		bgft ? dlsym(bgft, "sceBgftServiceDownloadRegisterTask") : NULL;
	int(*sceBgftDownloadRegisterTaskStore)(struct bgft_download_param*, int*, void*) =
		bgft ? dlsym(bgft, "sceBgftServiceIntDownloadRegisterTaskStore") : NULL;
	int(*sceBgftDebugDownloadRegisterTask)(struct bgft_download_param*, int*) =
		bgft ? dlsym(bgft, "sceBgftServiceIntDebugDownloadRegisterPkg") : NULL;
	int(*sceBgftDownloadStartTask)(int) =
		bgft ? dlsym(bgft, "sceBgftServiceIntDownloadStartTask") : NULL;
	int(*sceBgftPublicStartTask)(int) =
		bgft ? dlsym(bgft, "sceBgftServiceDownloadStartTask") : NULL;

	clear_stack();

	struct OrbisUserServiceInitializeParams init_params = {
		.priority = ORBIS_KERNEL_PRIO_FIFO_NORMAL
	};

	int uid = -1;
	if (sceUserServiceInitialize) sceUserServiceInitialize(&init_params);
	if (!sceUserServiceGetForegroundUser || sceUserServiceGetForegroundUser(&uid) != 0 || uid < 0) {
		if (sceUserServiceGetInitialUser) sceUserServiceGetInitialUser(&uid);
	}
	if (sceUserServiceTerminate) sceUserServiceTerminate();
	if (uid < 0) {
		if (sceSysUtilSendSystemNotificationWithText)
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Nenhum usuário ativo no PS4.");
		return -1;
	}

	void* libSceAppInstUtil = dlopen("/system/common/lib/libSceAppInstUtil.sprx", 0);
	int(*sceAppInstUtilInitialize)(void) =
		libSceAppInstUtil ? dlsym(libSceAppInstUtil, "sceAppInstUtilInitialize") : NULL;
	int(*sceAppInstUtilTerminate)(void) =
		libSceAppInstUtil ? dlsym(libSceAppInstUtil, "sceAppInstUtilTerminate") : NULL;

	if (!sceAppInstUtilInitialize) {
		if (sceSysUtilSendSystemNotificationWithText)
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Falha ao carregar AppInstUtil.");
		return -1;
	}

	int rv = sceAppInstUtilInitialize();
	if (rv) {
		char err[0x200] = "\x0";
		char errCode[0x20] = "\x0";
		int32ToHex(rv, errCode);
		concat("WILL Installer: AppInstUtil ", errCode, err);
		if (sceSysUtilSendSystemNotificationWithText) sceSysUtilSendSystemNotificationWithText(222, err);
		return -1;
	}

	if (!sceBgftInitialize || (!sceBgftDownloadStartTask && !sceBgftPublicStartTask)) {
		if (sceSysUtilSendSystemNotificationWithText)
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Falha ao carregar libSceBgft.");
		if (sceAppInstUtilTerminate) sceAppInstUtilTerminate();
		return -1;
	}

	void* bgft_mem = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (bgft_mem == NULL || bgft_mem == MAP_FAILED) {
		if (sceSysUtilSendSystemNotificationWithText)
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Sem memória para BGFT.");
		if (sceAppInstUtilTerminate) sceAppInstUtilTerminate();
		return -1;
	}

	struct bgft_init_params ip = {
		.mem = bgft_mem,
		.size = 0x100000,
	};

	rv = sceBgftInitialize(&ip);
	if (rv && (uint32_t)rv != 0x80990001u) {
		char err[0x200] = "\x0";
		char errCode[0x20] = "\x0";
		int32ToHex(rv, errCode);
		concat("WILL Installer: BGFT Init ", errCode, err);
		if (sceSysUtilSendSystemNotificationWithText) sceSysUtilSendSystemNotificationWithText(222, err);
		if (sceAppInstUtilTerminate) sceAppInstUtilTerminate();
		munmap(ip.mem, ip.size);
		return -1;
	}

	struct bgft_download_param bgft_params = {
		.user_id = uid,
		.entitlement_type = 5,
		.id = "",
		.content_url = "",
		.content_ex_url = NULL,
		.content_name = "",
		.icon_path = "",
		.sku_id = NULL,
		.option = BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM,
		.playgo_scenario_id = "0",
		.release_date = NULL,
		.package_type = "PS4GD",
		.package_sub_type = "",
		.package_size = 0
	};

	while (1) {
		rv = get_pkg_info(&bgft_params);
		if (rv == -1) {
			if (sceSysUtilSendSystemNotificationWithText)
				sceSysUtilSendSystemNotificationWithText(222, "WILL Installer: GET INFO ERROR");
			if (sceAppInstUtilTerminate) sceAppInstUtilTerminate();
			munmap(ip.mem, ip.size);
			return -1;
		}
		if (rv == 1) break;

		int is_manifest = url_has_json_ext(bgft_params.content_url);
		int task = BGFT_INVALID_TASK_ID;
		int registered = 0;
		unsigned char store_err[0x100];

		if (is_manifest && sceBgftDownloadRegisterTaskStore) {
			task = BGFT_INVALID_TASK_ID;
			for (int j = 0; j < 0x100; j++) store_err[j] = 0;
			rv = sceBgftDownloadRegisterTaskStore(&bgft_params, &task, store_err);
			if (rv >= 0 && task != BGFT_INVALID_TASK_ID) registered = 1;
		}

		if (!registered && sceBgftDownloadRegisterTask) {
			task = BGFT_INVALID_TASK_ID;
			rv = sceBgftDownloadRegisterTask(&bgft_params, &task);
			if (rv >= 0 && (uint32_t)rv != 0x80990088u && task != BGFT_INVALID_TASK_ID) registered = 1;
		}

		if (!registered && (uint32_t)rv != 0x80990088u && sceBgftDebugDownloadRegisterTask) {
			task = BGFT_INVALID_TASK_ID;
			rv = sceBgftDebugDownloadRegisterTask(&bgft_params, &task);
			if (rv >= 0 && (uint32_t)rv != 0x80990088u && task != BGFT_INVALID_TASK_ID) registered = 1;
		}

		if (!registered && !is_manifest && (uint32_t)rv != 0x80990088u && sceBgftDownloadRegisterTaskStore) {
			task = BGFT_INVALID_TASK_ID;
			for (int j = 0; j < 0x100; j++) store_err[j] = 0;
			rv = sceBgftDownloadRegisterTaskStore(&bgft_params, &task, store_err);
			if (rv >= 0 && task != BGFT_INVALID_TASK_ID) registered = 1;
		}

		if (registered) {
			int start_rv = sceBgftDownloadStartTask ? sceBgftDownloadStartTask(task) : -1;
			if (start_rv != 0 && sceBgftPublicStartTask) {
				start_rv = sceBgftPublicStartTask(task);
			}
			if (start_rv == 0) {
				if (sceSysUtilSendSystemNotificationWithText)
					sceSysUtilSendSystemNotificationWithText(222, "WILL: Download iniciado em Notificações!");
				break;
			}
			rv = start_rv;
			if (sceSysUtilSendSystemNotificationWithText) {
				char err[0x200] = "\x0";
				char errCode[0x20] = "\x0";
				int32ToHex(rv, errCode);
				concat("WILL: Falha ao iniciar tarefa ", errCode, err);
				sceSysUtilSendSystemNotificationWithText(222, err);
			}
			break;
		}

		if (!sceSysUtilSendSystemNotificationWithText) break;

		if ((uint32_t)rv == 0x80990086u) {
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Erro 0x80990086\nVerifique downloads antigos em Notificações.");
			break;
		}
		if ((uint32_t)rv == 0x80990088u) {
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Conteúdo já instalado ou já na lista de download!");
			break;
		}
		if ((uint32_t)rv == 0x80990039u || (uint32_t)rv == 0x80A30026u || (uint32_t)rv == 0x80990085u) {
			sceSysUtilSendSystemNotificationWithText(222, "WILL: Espaço insuficiente no PS4.");
			break;
		}
		if ((uint32_t)rv == 0x80990015u || (uint32_t)rv == 0x80990004u) {
			char err[0x200] = "\x0";
			char errCode[0x20] = "\x0";
			int32ToHex(rv, errCode);
			concat("WILL: Falha de rede ou rejeição ", errCode, err);
			sceSysUtilSendSystemNotificationWithText(222, err);
			break;
		}

		char err[0x200] = "\x0";
		char errCode[0x20] = "\x0";
		int32ToHex(rv, errCode);
		concat("WILL: Erro BGFT -> ", errCode, err);
		sceSysUtilSendSystemNotificationWithText(222, err);
		break;
	}

	// Mantém BGFT/memória vivos para não cancelar a tarefa recém-iniciada
	return 0;
}
