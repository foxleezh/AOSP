## 前言
上一篇中讲了init进程的第一阶段，我们接着讲第二阶段，主要有以下内容

- 创建进程会话密钥并初始化属性系统
- 进行SELinux第二阶段并恢复一些文件安全上下文
- 新建epoll并初始化子进程终止信号处理函数
- 设置其他系统属性并开启系统属性服务

本文涉及到的文件
```
platform/system/core/init/init.cpp
platform/system/core/init/keyutils.h
platform/system/core/init/property_service.cpp
platform/external/selinux/libselinux/src/label.c
platform/system/core/init/signal_handler.cpp
platform/system/core/init/service.cpp
platform/system/core/init/property_service.cpp
```

一、创建进程会话密钥并初始化属性系统

第二阶段一开始会有一个is_first_stage的判断，由于之前第一阶段最后有设置INIT_SECOND_STAGE，
因此直接跳过一大段代码。从keyctl开始才是重点内容，我们一一展开来看

```C
int main(int argc, char** argv) {

    //同样进行ueventd/watchdogd跳转及环境变量设置

    ...

    //之前准备工作时将INIT_SECOND_STAGE设置为true，已经不为nullptr，所以is_first_stage为false
    bool is_first_stage = (getenv("INIT_SECOND_STAGE") == nullptr);

    //is_first_stage为false，直接跳过
    if (is_first_stage) {
        ...
    }

    // At this point we're in the second stage of init.
    InitKernelLogging(argv); //上一节有讲，初始化日志输出
    LOG(INFO) << "init second stage started!";

    // Set up a session keyring that all processes will have access to. It
    // will hold things like FBE encryption keys. No process should override
    // its session keyring.
    keyctl(KEYCTL_GET_KEYRING_ID, KEY_SPEC_SESSION_KEYRING, 1); //初始化进程会话密钥

    // Indicate that booting is in progress to background fw loaders, etc.
    close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));//创建 /dev/.booting 文件，就是个标记，表示booting进行中

    property_init();//初始化属性系统，并从指定文件读取属性

    //接下来的一系列操作都是从各个文件读取一些属性，然后通过property_set设置系统属性

    // If arguments are passed both on the command line and in DT,
    // properties set in DT always have priority over the command-line ones.
    /*
     * 1.这句英文的大概意思是，如果参数同时从命令行和DT传过来，DT的优先级总是大于命令行的
     * 2.DT即device-tree，中文意思是设备树，这里面记录自己的硬件配置和系统运行参数，参考http://www.wowotech.net/linux_kenrel/why-dt.html
     */

    process_kernel_dt();//处理DT属性
    process_kernel_cmdline();//处理命令行属性

    // Propagate the kernel variables to internal variables
    // used by init as well as the current required properties.
    export_kernel_boot_props();//处理其他的一些属性

    // Make the time that init started available for bootstat to log.
    property_set("ro.boottime.init", getenv("INIT_STARTED_AT"));
    property_set("ro.boottime.init.selinux", getenv("INIT_SELINUX_TOOK"));

    // Set libavb version for Framework-only OTA match in Treble build.
    const char* avb_version = getenv("INIT_AVB_VERSION");
    if (avb_version) property_set("ro.boot.avb_version", avb_version);

    // Clean up our environment.
    unsetenv("INIT_SECOND_STAGE"); //清空这些环境变量，因为之前都已经存入到系统属性中去了
    unsetenv("INIT_STARTED_AT");
    unsetenv("INIT_SELINUX_TOOK");
    unsetenv("INIT_AVB_VERSION");

    ...

```

### 1.1 keyctl
定义在platform/system/core/init/keyutils.h

keyctl将主要的工作交给__NR_keyctl这个系统调用，keyctl是Linux系统操纵内核的通讯密钥管理工具


我们分析下 keyctl(KEYCTL_GET_KEYRING_ID, KEY_SPEC_SESSION_KEYRING, 1)

- KEYCTL_GET_KEYRING_ID 表示通过第二个参数的类型获取当前进程的密钥信息
- KEY_SPEC_SESSION_KEYRING 表示获取当前进程的SESSION_KEYRING（会话密钥环）
- 1 表示如果获取不到就新建一个

参考[linux手册](http://www.man7.org/linux/man-pages/man2/keyctl.2.html)

这里并没有拿返回值，估计就是为了新建会话密钥环了,从注释Set up a session keyring也可看出

```C
static inline long keyctl(int cmd, ...) {
    va_list va;
    unsigned long arg2, arg3, arg4, arg5;

    //va_start，va_arg,va_end是配合使用的，用于将可变参数从堆栈中读取出来
    va_start(va, cmd); //va_start是获取第一个参数地址
    arg2 = va_arg(va, unsigned long); //va_arg 遍历参数
    arg3 = va_arg(va, unsigned long);
    arg4 = va_arg(va, unsigned long);
    arg5 = va_arg(va, unsigned long);
    va_end(va); //va_end 恢复堆栈
    return syscall(__NR_keyctl, cmd, arg2, arg3, arg4, arg5); //系统调用
}
```


### 1.2 property_init
定义在 platform/system/core/init/property_service.cpp

直接交给 __system_property_area_init 处理

```C
void property_init() {
    if (__system_property_area_init()) {
        LOG(ERROR) << "Failed to initialize property area";
        exit(1);
    }
}
```

__system_property_area_init 定义在/bionic/libc/bionic/system_properties.cpp

看名字大概知道是用来初始化属性系统区域的，应该是分门别类更准确些，首先清除缓存，这里主要是清除几个链表以及在内存中的映射，新建property_filename目录，这个目录的值为 /dev/\__properties\__
然后就是调用initialize_properties加载一些系统属性的类别信息，最后将加载的链表写入文件并映射到内存


```C
int __system_property_area_init() {
  free_and_unmap_contexts();//清除一些缓存
  mkdir(property_filename, S_IRWXU | S_IXGRP | S_IXOTH);//新建目录property_filename，权限是rwx-x-x
  if (!initialize_properties()) { //读取一些文件，把键值信息存入到链表中
    return -1;
  }
  bool open_failed = false;
  bool fsetxattr_failed = false;
  list_foreach(contexts, [&fsetxattr_failed, &open_failed](context_node* l) {
    if (!l->open(true, &fsetxattr_failed)) {
    //将contexts链表中的数据写入到property_filename目录下文件中，每种context对应一个文件，并通过mmap映射进内存中
      open_failed = true;
    }
  });
  if (open_failed || !map_system_property_area(true, &fsetxattr_failed)) {//增加 properties_serial的映射，跟contexts中的一样
    free_and_unmap_contexts();//映射失败清除缓存
    return -1;
  }
  initialized = true;
  return fsetxattr_failed ? -2 : 0;
}
```

### 1.3 initialize_properties

定义在/bionic/libc/bionic/system_properties.cpp

交给 initialize_properties_from_file 处理，指定了一些文件路径

```C
static bool initialize_properties() {
  // If we do find /property_contexts, then this is being
  // run as part of the OTA updater on older release that had
  // /property_contexts - b/34370523
  if (initialize_properties_from_file("/property_contexts")) {
    return true;
  }

  // Use property_contexts from /system & /vendor, fall back to those from /
  if (access("/system/etc/selinux/plat_property_contexts", R_OK) != -1) {
    if (!initialize_properties_from_file("/system/etc/selinux/plat_property_contexts")) {
      return false;
    }
    // Don't check for failure here, so we always have a sane list of properties.
    // E.g. In case of recovery, the vendor partition will not have mounted and we
    // still need the system / platform properties to function.
    initialize_properties_from_file("/vendor/etc/selinux/nonplat_property_contexts");
  } else {
    if (!initialize_properties_from_file("/plat_property_contexts")) {
      return false;
    }
    initialize_properties_from_file("/nonplat_property_contexts");
  }

  return true;
}
```


### 1.4 initialize_properties_from_file

定义在/bionic/libc/bionic/system_properties.cpp

这个函数主要工作是解析属性类别文件，对属性做一下分类，具体就是一行行解析，过滤 # 开头的、只读到key的、从ctl.开头的，然后将解析出来的键值对放到两个链表中

prefixes链表存放key(其实是一些key的前缀),contexts链表存放value(其实是对应key应当属于那些类别的信息)，这样的好处是将庞杂的属性根据前缀分类，存储到不同的context中，
查找和修改是非常高效的，类似map的做法

```C
static bool initialize_properties_from_file(const char* filename) {
  FILE* file = fopen(filename, "re");
  if (!file) {
    return false;
  }

  char* buffer = nullptr;
  size_t line_len;
  char* prop_prefix = nullptr;
  char* context = nullptr;

  while (getline(&buffer, &line_len, file) > 0) { //一行一行读取，然后将结果放到buffer中
    int items = read_spec_entries(buffer, 2, &prop_prefix, &context);
    //将buffer的数据，按空格作为区分，key赋值给prop_prefix，value赋值给context

    if (items <= 0) { //没有读取到，比如 # 这种是注释
      continue;
    }
    if (items == 1) { //只读取到key，释放key的内存
      free(prop_prefix);
      continue;
    }
    /*
     * init uses ctl.* properties as an IPC mechanism and does not write them
     * to a property file, therefore we do not need to create property files
     * to store them.
     */
    if (!strncmp(prop_prefix, "ctl.", 4)) { //以ctl.开头忽略掉，因为这个不属于属性，主要用于IPC机制
      free(prop_prefix);
      free(context);
      continue;
    }

    /*
     * C++中[ arg1,arg2,... ](T param, T param1,... ){ commond} 这个是lambda表达式，也可以看作一个函数指针
     * []中是引用外部参数
     *（）中是参数定义，这个跟普通方法的（）一样
     * {}中是方法体
     */
    auto old_context =
        list_find(contexts, [context](context_node* l) { return !strcmp(l->context(), context); });

    // list_find主要是循环contexts这个链表，如果发现context的值在链表里已经有，就将对应的链表结构context_node返回
    if (old_context) {
      list_add_after_len(&prefixes, prop_prefix, old_context);
      //list_add_after_len 主要作用是将prop_prefix和old_context按顺序放到prefixes链表里
    } else {
      list_add(&contexts, context, nullptr);//将context的值放到contexts链表里
      list_add_after_len(&prefixes, prop_prefix, contexts);
    }
    free(prop_prefix); //释放资源
    free(context);
  }

  free(buffer);
  fclose(file);

  return true;
}
```

### 1.5 链表结构

定义在/bionic/libc/bionic/system_properties.cpp

之前我们看到有两个重要的链表prefixs和contexts,frefixs存key(其实是一些key的前缀),contexts存value(其实是对应key应当属于那些类别的信息),接下来我们看下这两个链表的结构

context_node中有三个比较重要的属性context_、_pa和next，context_用来存类别信息，_pa是存具体key-value节点的,next是链表下一个节点

prefix_node中有三个重要属性prefix，context和next，prefix用来存key,context用来存关联的context_node，next是链表下一个节点

prop_area 这个在context_node里引用，属性data是具体key-value的数据库，里面是用 hybrid trie/binary tree（字典树）这种结构存储的，也就是一对多，我给张图就明白了

![](http://upload-images.jianshu.io/upload_images/3387045-a8453c6017bbf63f.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)


这种结构的好处是查找快，就像我们电脑里的目录一样，prop_area是个数据库，里面的基本结构是prop_bt，prop_bt里面有name,left,right,children,prop,其中prop是prop_info结构

prop_info 就是具体的key-value了，我们键值对信息就存在这里面

```C
class context_node {
 public:
  /*
   * C++中构造函数后面接 ：（冒号） 表示对属性赋初始值
   */
  context_node(context_node* next, const char* context, prop_area* pa)
      : next(next), context_(strdup(context)), pa_(pa), no_access_(false) {

    lock_.init(false);
  }

  ...

  context_node* next;

 private:
  bool check_access();
  void unmap();

  Lock lock_;
  char* context_;
  prop_area* pa_;
  bool no_access_;
};

struct prefix_node {
  prefix_node(struct prefix_node* next, const char* prefix, context_node* context)
      : prefix(strdup(prefix)), prefix_len(strlen(prefix)), context(context), next(next) {
  }
  ~prefix_node() {
    free(prefix);
  }
  char* prefix;
  const size_t prefix_len;
  context_node* context;
  struct prefix_node* next;
};

class prop_area {
 public:
  prop_area(const uint32_t magic, const uint32_t version) : magic_(magic), version_(version) {
    atomic_init(&serial_, 0);
    memset(reserved_, 0, sizeof(reserved_));
    // Allocate enough space for the root node.
    bytes_used_ = sizeof(prop_bt);
  }

  ....

 private:

  ...

  uint32_t bytes_used_;
  atomic_uint_least32_t serial_;
  uint32_t magic_;
  uint32_t version_;
  uint32_t reserved_[28];
  char data_[0];

  DISALLOW_COPY_AND_ASSIGN(prop_area);
};


struct prop_bt {
  uint32_t namelen;
  atomic_uint_least32_t prop;
  atomic_uint_least32_t left;
  atomic_uint_least32_t right;
  atomic_uint_least32_t children;
  char name[0];
};


struct prop_info {
  atomic_uint_least32_t serial;
  // we need to keep this buffer around because the property
  // value can be modified whereas name is constant.
  char value[PROP_VALUE_MAX];
  char name[0];

  prop_info(const char* name, uint32_t namelen, const char* value, uint32_t valuelen) {
    memcpy(this->name, name, namelen);
    this->name[namelen] = '\0';
    atomic_init(&this->serial, valuelen << 24);
    memcpy(this->value, value, valuelen);
    this->value[valuelen] = '\0';
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(prop_info);
};
```

之前有个list_add函数，这个函数是一个模板函数，与Java中的泛型类似，List 和 Args相当于T和T1,这个函数主要作用就是调用T的构造函数，
把list,可变参数args作为参数传进去

```C

template <typename List, typename... Args>
static inline void list_add(List** list, Args... args) {
  *list = new List(*list, args...);
}

```

### 1.6 process_kernel_dt
定义在platform/system/core/init/init.cpp

读取DT（设备树）的属性信息，然后通过 property_set 设置系统属性

```C
static void process_kernel_dt() {
    if (!is_android_dt_value_expected("compatible", "android,firmware")) {
    //判断 /proc/device-tree/firmware/android/compatible 文件中的值是否为 android,firmware
        return;
    }

    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(kAndroidDtDir.c_str()), closedir);
    // kAndroidDtDir的值为/proc/device-tree/firmware/android

    if (!dir) return;

    std::string dt_file;
    struct dirent *dp;
    while ((dp = readdir(dir.get())) != NULL) { //遍历dir中的文件
        if (dp->d_type != DT_REG || !strcmp(dp->d_name, "compatible") || !strcmp(dp->d_name, "name")) {
            //跳过 compatible和name文件
            continue;
        }

        std::string file_name = kAndroidDtDir + dp->d_name;

        android::base::ReadFileToString(file_name, &dt_file); //读取文件内容
        std::replace(dt_file.begin(), dt_file.end(), ',', '.'); //替换 , 为 .

        std::string property_name = StringPrintf("ro.boot.%s", dp->d_name);
        property_set(property_name.c_str(), dt_file.c_str()); // 将 ro.boot.文件名 作为key，文件内容为value，设置进属性
    }
}
```

### 1.7 property_set
定义在/bionic/libc/bionic/system_properties.cpp

property_set用的地方特别多，作用是设置系统属性，具体就是通过遍历之前的prefixs链表找到对应的context_node,然后通过context_node的_pa属性找到对应key-value节点prop_info，能找到就更新value，找不到就设置新值，
另外就是调用property_changed方法触发trigger，trigger后续讲.rc解析时再详细讲，trigger可以触发一系列活动

```C
uint32_t property_set(const std::string& name, const std::string& value) {
    size_t valuelen = value.size();

    if (!is_legal_property_name(name)) { //检查key合法性，大概就是 xx.xx.xx 这种 ，xx只能是字母、数字、_、-、@
        LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: bad name";
        return PROP_ERROR_INVALID_NAME;
    }

    if (valuelen >= PROP_VALUE_MAX) {//不能超过最大长度 92
        LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                   << "value too long";
        return PROP_ERROR_INVALID_VALUE;
    }

    if (name == "selinux.restorecon_recursive" && valuelen > 0) { // 跳过selinux，不允许修改
        if (restorecon(value.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
            LOG(ERROR) << "Failed to restorecon_recursive " << value;
        }
    }

    prop_info* pi = (prop_info*) __system_property_find(name.c_str()); //找到key对应节点
    if (pi != nullptr) { //如果对应节点存在就更新
        // ro.* properties are actually "write-once".
        if (android::base::StartsWith(name, "ro.")) {
            LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                       << "property already set";
            return PROP_ERROR_READ_ONLY_PROPERTY;
        }

        __system_property_update(pi, value.c_str(), valuelen);
    } else { //没有对应节点就新建
        int rc = __system_property_add(name.c_str(), name.size(), value.c_str(), valuelen);
        if (rc < 0) {
            LOG(ERROR) << "property_set(\"" << name << "\", \"" << value << "\") failed: "
                       << "__system_property_add failed";
            return PROP_ERROR_SET_FAILED;
        }
    }

    // Don't write properties to disk until after we have read all default
    // properties to prevent them from being overwritten by default values.
    if (persistent_properties_loaded && android::base::StartsWith(name, "persist.")) {
    //如果以persist开头的，将值写入文件
        write_persistent_property(name.c_str(), value.c_str());
    }
    property_changed(name, value); //触发trigger
    return PROP_SUCCESS;
}
```

### 1.8 其他属性设置

后续的一些函数或代码都是直接或间接调用 property_set 设置系统属性

```C
static void process_kernel_cmdline() {
    // The first pass does the common stuff, and finds if we are in qemu.
    // The second pass is only necessary for qemu to export all kernel params
    // as properties.
    import_kernel_cmdline(false, import_kernel_nv);
    if (qemu[0]) import_kernel_cmdline(true, import_kernel_nv);
}

static void import_kernel_nv(const std::string& key, const std::string& value, bool for_emulator) {
    if (key.empty()) return;

    if (for_emulator) {
        // In the emulator, export any kernel option with the "ro.kernel." prefix.
        property_set(StringPrintf("ro.kernel.%s", key.c_str()).c_str(), value.c_str());
        return;
    }

    if (key == "qemu") {
        strlcpy(qemu, value.c_str(), sizeof(qemu));
    } else if (android::base::StartsWith(key, "androidboot.")) {
        property_set(StringPrintf("ro.boot.%s", key.c_str() + 12).c_str(), value.c_str());
    }
}

```

```C
static void export_kernel_boot_props() {
    struct {
        const char *src_prop;
        const char *dst_prop;
        const char *default_value;
    } prop_map[] = {
        { "ro.boot.serialno",   "ro.serialno",   "", },
        { "ro.boot.mode",       "ro.bootmode",   "unknown", },
        { "ro.boot.baseband",   "ro.baseband",   "unknown", },
        { "ro.boot.bootloader", "ro.bootloader", "unknown", },
        { "ro.boot.hardware",   "ro.hardware",   "unknown", },
        { "ro.boot.revision",   "ro.revision",   "0", },
    };
    for (size_t i = 0; i < arraysize(prop_map); i++) {
        std::string value = GetProperty(prop_map[i].src_prop, "");
        property_set(prop_map[i].dst_prop, (!value.empty()) ? value.c_str() : prop_map[i].default_value);
    }
}
```

## 二、进行SELinux第二阶段并恢复一些文件安全上下文

```C
    // Now set up SELinux for second stage.
    selinux_initialize(false); //第二阶段初始化SELinux policy
    selinux_restore_context();//恢复安全上下文
```

### 2.1 selinux_initialize
定义在platform/system/core/init/init.cpp

第二阶段只是执行 selinux_init_all_handles

```C
static void selinux_initialize(bool in_kernel_domain) {

    ... //和之前一样设置回调函数

    if (in_kernel_domain) {//第二阶段跳过
       ...
    } else {
        selinux_init_all_handles();
    }
}
```

### 2.2 selinux_init_all_handles
定义在platform/system/core/init/init.cpp

这里是创建SELinux的处理函数，selinux_android_file_context_handle和selinux_android_prop_context_handle内部实现差不多，其实就是传递不同的文件路径给selabel_open

```C
static void selinux_init_all_handles(void)
{
    sehandle = selinux_android_file_context_handle(); //创建context的处理函数
    selinux_android_set_sehandle(sehandle);//将刚刚新建的处理赋值给fc_sehandle
    sehandle_prop = selinux_android_prop_context_handle();//创建prop的处理函数
}
```

### 2.2 selabel_open

定义在platform/external/selinux/libselinux/src/label.c

首先创建一个selabel_handle结构体，然后根据backend的类型将处理函数映射给initfuncs数组中的值，将参数opts传递过去

这个opts只是包含一个简单的路径，比如 /system/etc/selinux/plat_file_contexts ，而initfuncs负责去解析它

```C
struct selabel_handle *selabel_open(unsigned int backend,
				    const struct selinux_opt *opts,
				    unsigned nopts)
{
	struct selabel_handle *rec = NULL;

	if (backend >= ARRAY_SIZE(initfuncs)) {
		errno = EINVAL;
		goto out;
	}

	if (!initfuncs[backend]) {
		errno = ENOTSUP;
		goto out;
	}

	rec = (struct selabel_handle *)malloc(sizeof(*rec));
	if (!rec)
		goto out;

	memset(rec, 0, sizeof(*rec));
	rec->backend = backend;
	rec->validating = selabel_is_validate_set(opts, nopts);

	rec->subs = NULL;
	rec->dist_subs = NULL;
	rec->digest = selabel_is_digest_set(opts, nopts, rec->digest);

	if ((*initfuncs[backend])(rec, opts, nopts)) { //
		selabel_close(rec);
		rec = NULL;
	}
out:
	return rec;
}
```

### 2.3 initfuncs
定义在platform/external/selinux/libselinux/src/label.c

这些数组对应backend的6种可能的值
```C
/* file contexts */
#define SELABEL_CTX_FILE	0
/* media contexts */
#define SELABEL_CTX_MEDIA	1
/* x contexts */
#define SELABEL_CTX_X		2
/* db objects */
#define SELABEL_CTX_DB		3
/* Android property service contexts */
#define SELABEL_CTX_ANDROID_PROP 4
/* Android service contexts */
#define SELABEL_CTX_ANDROID_SERVICE 5
```

initfuncs数组中每一项都对应一个init函数，init函数主要作用是解析传进来的文件，这些传进来的文件定义了哪些进程可以访问哪些文件，执行哪些操作
SELinux的内容比较多，由于篇幅就暂时不深入了
可以参考老罗的[SEAndroid安全机制框架分析](http://blog.csdn.net/luoshengyang/article/details/37613135)

```C
static selabel_initfunc initfuncs[] = {
	&selabel_file_init,
	CONFIG_MEDIA_BACKEND(selabel_media_init),
	CONFIG_X_BACKEND(selabel_x_init),
	CONFIG_DB_BACKEND(selabel_db_init),
	CONFIG_ANDROID_BACKEND(selabel_property_init),
	CONFIG_ANDROID_BACKEND(selabel_service_init),
};
```

### 2.3 selinux_restore_context
定义在 platform/system/core/init/init.cpp


```C
static void selinux_restore_context() {
    LOG(INFO) << "Running restorecon...";
    restorecon("/dev");
    restorecon("/dev/kmsg");
    restorecon("/dev/socket");
    restorecon("/dev/random");
    restorecon("/dev/urandom");
    restorecon("/dev/__properties__");

    restorecon("/file_contexts.bin");
    restorecon("/plat_file_contexts");
    restorecon("/nonplat_file_contexts");
    restorecon("/plat_property_contexts");
    restorecon("/nonplat_property_contexts");
    restorecon("/plat_seapp_contexts");
    restorecon("/nonplat_seapp_contexts");
    restorecon("/plat_service_contexts");
    restorecon("/nonplat_service_contexts");
    restorecon("/plat_hwservice_contexts");
    restorecon("/nonplat_hwservice_contexts");
    restorecon("/sepolicy");
    restorecon("/vndservice_contexts");

    restorecon("/sys", SELINUX_ANDROID_RESTORECON_RECURSE);
    restorecon("/dev/block", SELINUX_ANDROID_RESTORECON_RECURSE);
    restorecon("/dev/device-mapper");
}
```

主要就是恢复这些文件的安全上下文，因为这些文件是在SELinux安全机制初始化前创建，所以需要重新恢复下安全性

## 三、新建epoll并初始化子进程终止信号处理函数

```C

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);//创建epoll实例，并返回epoll的文件描述符

    if (epoll_fd == -1) {
        PLOG(ERROR) << "epoll_create1 failed";
        exit(1);
    }

    signal_handler_init();//主要是创建handler处理子进程终止信号，创建一个匿名socket并注册到epoll进行监听

```

### 3.1 epoll_create1
EPOLL类似于POLL，是Linux中用来做事件触发的，跟EventBus功能差不多

linux很长的时间都在使用select来做事件触发，它是通过轮询来处理的，轮询的fd数目越多，自然耗时越多，对于大量的描述符处理，EPOLL更有优势

epoll_create1是epoll_create的升级版，可以动态调整epoll实例中文件描述符的个数
EPOLL_CLOEXEC这个参数是为文件描述符添加O_CLOEXEC属性，参考http://blog.csdn.net/gqtcgq/article/details/48767691

### 3.2 signal_handler_init
定义在platform/system/core/init/signal_handler.cpp


```C
void signal_handler_init() {
    // Create a signalling mechanism for SIGCHLD.
    int s[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, s) == -1) { //创建socket并返回文件描述符
        PLOG(ERROR) << "socketpair failed";
        exit(1);
    }

    signal_write_fd = s[0];
    signal_read_fd = s[1];

    // Write to signal_write_fd if we catch SIGCHLD.
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIGCHLD_handler; //act处理函数
    act.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &act, 0);

    ServiceManager::GetInstance().ReapAnyOutstandingChildren();//具体处理子进程终止信号

    register_epoll_handler(signal_read_fd, handle_signal);//注册signal_read_fd到epoll中
}


void register_epoll_handler(int fd, void (*fn)()) {
    epoll_event ev;
    ev.events = EPOLLIN; //监听事件类型，EPOLLIN表示fd中有数据可读
    ev.data.ptr = reinterpret_cast<void*>(fn); //回调函数赋值给ptr
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) { //注册事件
        PLOG(ERROR) << "epoll_ctl failed";
    }
}
```

这个函数主要的作用是注册SIGCHLD信号的处理函数

init是一个守护进程，为了防止init的子进程成为僵尸进程(zombie process)，
需要init在子进程在结束时获取子进程的结束码，通过结束码将程序表中的子进程移除，
防止成为僵尸进程的子进程占用程序表的空间（程序表的空间达到上限时，系统就不能再启动新的进程了，会引起严重的系统问题）

在linux当中，父进程是通过捕捉SIGCHLD信号来得知子进程运行结束的情况，SIGCHLD信号会在子进程终止的时候发出，了解这些背景后，我们来看看init进程如何处理这个信号

首先，调用socketpair,这个方法会返回一对文件描述符，这样当一端写入时，另一端就能被通知到，
socketpair两端既可以写也可以读，这里只是单向的让s[0]写，s[1]读

然后，新建一个sigaction结构体，sa_handler是信号处理函数，指向SIGCHLD_handler，
SIGCHLD_handler做的事情就是往s[0]里写个"1"，这样s[1](signal_read_fd)就会收到通知，SA_NOCLDSTOP表示只在子进程终止时处理，
因为子进程在暂停时也会发出SIGCHLD信号

sigaction(SIGCHLD, &act, 0) 这个是建立信号绑定关系，也就是说当监听到SIGCHLD信号时，由act这个sigaction结构体处理

ReapAnyOutstandingChildren 这个后文讲

最后，register_epoll_handler的作用就是注册一个监听，当signal_read_fd（之前的s[1]）收到信号，触发handle_signal

终上所述，signal_handler_init函数的作用就是，接收到SIGCHLD信号时触发handle_signal

### 3.3 handle_signal
定义在platform/system/core/init/signal_handler.cpp


```C
static void handle_signal() {
    // Clear outstanding requests.
    char buf[32];
    read(signal_read_fd, buf, sizeof(buf));

    ServiceManager::GetInstance().ReapAnyOutstandingChildren();
}
```

首先清空signal_read_fd中的数据，然后调用ReapAnyOutstandingChildren，之前在signal_handler_init中调用过一次，
它其实是调用ReapOneProcess

### 3.4 ReapOneProcess
定义在platform/system/core/init/service.cpp

```C
bool ServiceManager::ReapOneProcess() {
    int status;
    pid_t pid = TEMP_FAILURE_RETRY(waitpid(-1, &status, WNOHANG));
    //用waitpid函数获取状态发生变化的子进程pid
    //waitpid的标记为WNOHANG，即非阻塞，返回为正值就说明有进程挂掉了

    if (pid == 0) {
        return false;
    } else if (pid == -1) {
        PLOG(ERROR) << "waitpid failed";
        return false;
    }

    Service* svc = FindServiceByPid(pid);//通过pid找到对应的Service

    std::string name;
    std::string wait_string;
    if (svc) {
        name = android::base::StringPrintf("Service '%s' (pid %d)",
                                           svc->name().c_str(), pid);
        if (svc->flags() & SVC_EXEC) {
            wait_string =
                android::base::StringPrintf(" waiting took %f seconds", exec_waiter_->duration_s());
        }
    } else {
        name = android::base::StringPrintf("Untracked pid %d", pid);
    }

    if (WIFEXITED(status)) {
        LOG(INFO) << name << " exited with status " << WEXITSTATUS(status) << wait_string;
    } else if (WIFSIGNALED(status)) {
        LOG(INFO) << name << " killed by signal " << WTERMSIG(status) << wait_string;
    } else if (WIFSTOPPED(status)) {
        LOG(INFO) << name << " stopped by signal " << WSTOPSIG(status) << wait_string;
    } else {
        LOG(INFO) << name << " state changed" << wait_string;
    }

    if (!svc) { //没有找到，说明已经结束了
        return true;
    }

    svc->Reap();//清除子进程相关的资源

    if (svc->flags() & SVC_EXEC) {
        exec_waiter_.reset();
    }
    if (svc->flags() & SVC_TEMPORARY) {
        RemoveService(*svc);
    }

    return true;
}
```

这是最终的处理函数了，这个函数先用waitpid找出挂掉进程的pid,然后根据pid找到对应Service，
最后调用Service的Reap方法清除资源,根据进程对应的类型，决定是否重启机器或重启进程

## 四、设置其他系统属性并开启系统属性服务

```C
    property_load_boot_defaults();//从文件中加载一些属性，读取usb配置
    export_oem_lock_status();//设置ro.boot.flash.locked 属性
    start_property_service();//开启一个socket监听系统属性的设置
    set_usb_controller();//设置sys.usb.controller 属性

```

### 4.1 设置其他系统属性


```C
void property_load_boot_defaults() {
    if (!load_properties_from_file("/system/etc/prop.default", NULL)) { //从文件中读取属性
        // Try recovery path
        if (!load_properties_from_file("/prop.default", NULL)) {
            // Try legacy path
            load_properties_from_file("/default.prop", NULL);
        }
    }
    load_properties_from_file("/odm/default.prop", NULL);
    load_properties_from_file("/vendor/default.prop", NULL);

    update_sys_usb_config();
}

static void export_oem_lock_status() {
    if (!android::base::GetBoolProperty("ro.oem_unlock_supported", false)) {
        return;
    }

    std::string value = GetProperty("ro.boot.verifiedbootstate", "");

    if (!value.empty()) {
        property_set("ro.boot.flash.locked", value == "orange" ? "0" : "1");
    }
}

static void set_usb_controller() {
    std::unique_ptr<DIR, decltype(&closedir)>dir(opendir("/sys/class/udc"), closedir);
    if (!dir) return;

    dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        if (dp->d_name[0] == '.') continue;

        property_set("sys.usb.controller", dp->d_name);
        break;
    }
}
```
property_load_boot_defaults，export_oem_lock_status，set_usb_controller这三个函数都是调用property_set设置一些系统属性

### 4.2 start_property_service
定义在platform/system/core/init/property_service.cpp


```C
void start_property_service() {
    property_set("ro.property_service.version", "2");

    property_set_fd = create_socket(PROP_SERVICE_NAME, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                    0666, 0, 0, NULL);//创建socket用于通信
    if (property_set_fd == -1) {
        PLOG(ERROR) << "start_property_service socket creation failed";
        exit(1);
    }

    listen(property_set_fd, 8);//监听property_set_fd，设置最大并发数为8

    register_epoll_handler(property_set_fd, handle_property_set_fd);//注册epoll事件
}

```

之前我们看到通过property_set可以轻松设置系统属性，那干嘛这里还要启动一个属性服务呢？这里其实涉及到一些权限的问题，不是所有进程都可以随意修改任何的系统属性，
Android将属性的设置统一交由init进程管理，其他进程不能直接修改属性，而只能通知init进程来修改，而在这过程中，init进程可以进行权限控制，我们来看看这些是如何实现的

首先创建一个socket并返回文件描述符，然后设置最大并发数为8，其他进程可以通过这个socket通知init进程修改系统属性，
最后注册epoll事件，也就是当监听到property_set_fd改变时调用handle_property_set_fd

### 4.3 handle_property_set_fd
定义在platform/system/core/init/property_service.cpp



```C
static void handle_property_set_fd() {
    static constexpr uint32_t kDefaultSocketTimeout = 2000; /* ms */

    int s = accept4(property_set_fd, nullptr, nullptr, SOCK_CLOEXEC);//等待客户端连接
    if (s == -1) {
        return;
    }

    struct ucred cr;
    socklen_t cr_size = sizeof(cr);
    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cr_size) < 0) {//获取连接到此socket的进程的凭据
        close(s);
        PLOG(ERROR) << "sys_prop: unable to get SO_PEERCRED";
        return;
    }

    SocketConnection socket(s, cr);// 建立socket连接
    uint32_t timeout_ms = kDefaultSocketTimeout;

    uint32_t cmd = 0;
    if (!socket.RecvUint32(&cmd, &timeout_ms)) { //读取socket中的操作信息
        PLOG(ERROR) << "sys_prop: error while reading command from the socket";
        socket.SendUint32(PROP_ERROR_READ_CMD);
        return;
    }

    switch (cmd) { //根据操作信息，执行对应处理,两者区别一个是以char形式读取，一个以String形式读取
    case PROP_MSG_SETPROP: {
        char prop_name[PROP_NAME_MAX];
        char prop_value[PROP_VALUE_MAX];

        if (!socket.RecvChars(prop_name, PROP_NAME_MAX, &timeout_ms) ||
            !socket.RecvChars(prop_value, PROP_VALUE_MAX, &timeout_ms)) {
          PLOG(ERROR) << "sys_prop(PROP_MSG_SETPROP): error while reading name/value from the socket";
          return;
        }

        prop_name[PROP_NAME_MAX-1] = 0;
        prop_value[PROP_VALUE_MAX-1] = 0;

        handle_property_set(socket, prop_value, prop_value, true);
        break;
      }

    case PROP_MSG_SETPROP2: {
        std::string name;
        std::string value;
        if (!socket.RecvString(&name, &timeout_ms) ||
            !socket.RecvString(&value, &timeout_ms)) {
          PLOG(ERROR) << "sys_prop(PROP_MSG_SETPROP2): error while reading name/value from the socket";
          socket.SendUint32(PROP_ERROR_READ_DATA);
          return;
        }

        handle_property_set(socket, name, value, false);
        break;
      }

    default:
        LOG(ERROR) << "sys_prop: invalid command " << cmd;
        socket.SendUint32(PROP_ERROR_INVALID_CMD);
        break;
    }
}
```

这个函数主要作用是建立socket连接，然后从socket中读取操作信息，根据不同的操作类型，调用handle_property_set做具体的操作

### 4.4 handle_property_set
定义在platform/system/core/init/property_service.cpp



```C
static void handle_property_set(SocketConnection& socket,
                                const std::string& name,
                                const std::string& value,
                                bool legacy_protocol) {
  const char* cmd_name = legacy_protocol ? "PROP_MSG_SETPROP" : "PROP_MSG_SETPROP2";
  if (!is_legal_property_name(name)) { //检查key的合法性
    LOG(ERROR) << "sys_prop(" << cmd_name << "): illegal property name \"" << name << "\"";
    socket.SendUint32(PROP_ERROR_INVALID_NAME);
    return;
  }

  struct ucred cr = socket.cred(); //获取操作进程的凭证
  char* source_ctx = nullptr;
  getpeercon(socket.socket(), &source_ctx);

  if (android::base::StartsWith(name, "ctl.")) { //如果以ctl.开头，就执行Service的一些控制操作
    if (check_control_mac_perms(value.c_str(), source_ctx, &cr)) {//SELinux安全检查，有权限才进行操作
      handle_control_message(name.c_str() + 4, value.c_str());
      if (!legacy_protocol) {
        socket.SendUint32(PROP_SUCCESS);
      }
    } else {
      LOG(ERROR) << "sys_prop(" << cmd_name << "): Unable to " << (name.c_str() + 4)
                 << " service ctl [" << value << "]"
                 << " uid:" << cr.uid
                 << " gid:" << cr.gid
                 << " pid:" << cr.pid;
      if (!legacy_protocol) {
        socket.SendUint32(PROP_ERROR_HANDLE_CONTROL_MESSAGE);
      }
    }
  } else { //其他的属性调用property_set进行设置
    if (check_mac_perms(name, source_ctx, &cr)) {//SELinux安全检查，有权限才进行操作
      uint32_t result = property_set(name, value);
      if (!legacy_protocol) {
        socket.SendUint32(result);
      }
    } else {
      LOG(ERROR) << "sys_prop(" << cmd_name << "): permission denied uid:" << cr.uid << " name:" << name;
      if (!legacy_protocol) {
        socket.SendUint32(PROP_ERROR_PERMISSION_DENIED);
      }
    }
  }

  freecon(source_ctx);
}
```

这就是最终的处理函数，以"ctl."开头的key就做一些Service的Start,Stop,Restart操作，其他的就是调用property_set进行属性设置，
不管是前者还是后者，都要进行SELinux安全性检查，只有该进程有操作权限才能执行相应操作

**小结**

init进程第二阶段主要工作是初始化属性系统，解析SELinux的匹配规则，处理子进程终止信号，启动系统属性服务，可以说每一项都很关键，如果说第一阶段是为属性系统，SELinux做准备，那么第二阶段就是真正去把这些落实的，下一篇我们将讲解.rc文件的解析