#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/wakelock.h>
#include <linux/mux.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/random.h>
#include <linux/time.h>
#include <linux/compiler.h>

#include <mach/gpio.h>
#include <mach/platform.h>
#include <mach/balong_power.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <linux/skbuff.h>

#include <mach/k3v2_s10_iomux_blocks_name.h>
extern int iomux_block_set_work_mode(char* block_name);
extern int iomux_block_set_lowpower_mode(char* block_name);
extern int iomux_block_set_gpio_mode(char* block_name);



MODULE_LICENSE("GPL"); 

#define ECHI_DEVICE_FILE        "/sys/devices/platform/hisik3-ehci/ehci_power"

#define MAX_ENUM_WAIT           1000  	/* mini sec */
#define MAX_ENUM_RETRY          20
#define MAX_HSIC_RESET_RETRY    3
#define MAX_WAIT_TIMEOUT		30000
#define MAX_DELAY_VALUE			10000

typedef enum ENUM_MODEM_STATE {
    MODEM_STATE_OFF=0,
    MODEM_STATE_POWER,
    MODEM_STATE_READY,
    MODEM_STATE_INVALID,
} ENUM_MODEM_STATE_T;

const char* const modem_state_str[] = {
    "MODEM_STATE_OFF",
    "MODEM_STATE_POWER",
    "MODEM_STATE_READY",
    "MODEM_STATE_INVALID",  //Invalid case
};

#define GPIO_CP_PM_INIT  0
#define GPIO_CP_PM_RDY  ( !(GPIO_CP_PM_INIT) )

#define GPIO_CP_SW_INIT  1
#define GPIO_CP_SW_RDY  ( !(GPIO_CP_SW_INIT) )

/* Enum of power state */
enum balong_power_s_e {
	BALONG_POW_S_OFF,
	BALONG_POW_S_INIT_ON,
	BALONG_POW_S_WAIT_READY,
	BALONG_POW_S_INIT_USB_ON,
	BALONG_POW_S_USB_ENUM_DONE,
	BALONG_POW_S_USB_L0,
	BALONG_POW_S_USB_L2,
	BALONG_POW_S_USB_L2_TO_L0,
	BALONG_POW_S_USB_L3,
	BALONG_POW_S_USB_L3_ENUM,
	BALONG_POW_S_MAX,
};

/* It's useful to debug by using the string of state instead of the number */
static const char *power_state_name[] = 
{
"POWER_OFF",
"INIT_ON",
"WAIT_READY",
"INIT_USB_ON",
"USB_ENUM_DONE",
"USB_L0",
"USB_L2",
"USB_L2_TO_L0",
"USB_L3",
"USB_L3_ENUM",
};
	
static enum balong_power_s_e  balong_curr_power_state;
static struct wake_lock balong_wakelock;
static struct workqueue_struct *workqueue;

static struct work_struct l2_resume_work;
static struct work_struct l3_resume_work;
static struct work_struct modem_reset_work;		
static struct work_struct modem_ready_work; 
static wait_queue_head_t  balong_wait_q;
static spinlock_t	  balong_state_lock;
static unsigned long      balong_last_suspend = 0;	

static int modem_on_delay = 140;
static int usb_on_delay = 100;
static int reset_isr_register = 0;		

static struct balong_power_plat_data* balong_driver_plat_data;

 


static struct work_struct usb_on_work;
static struct work_struct modem_on_work;
static int usb_enum_check(void);

/* Begin:  modify for mmc392 by 00176579, 2012-8-10*/
//#define GPIO_CP_RESET     		balong_driver_plat_data->gpios[BALONG_RESET].gpio
/* End:  modify for mmc392 by 00176579, 2012-8-10*/
#define GPIO_CP_POWER_ON     	balong_driver_plat_data->gpios[BALONG_POWER_ON].gpio
#define GPIO_CP_PMU_RESET     	balong_driver_plat_data->gpios[BALONG_PMU_RESET].gpio
#define GPIO_HOST_ACTIVE     	balong_driver_plat_data->gpios[BALONG_HOST_ACTIVE].gpio
#define GPIO_HOST_WAKEUP        balong_driver_plat_data->gpios[BALONG_HOST_WAKEUP].gpio
#define GPIO_SLAVE_WAKEUP       balong_driver_plat_data->gpios[BALONG_SLAVE_WAKEUP].gpio

#define GPIO_POWER_IND          	balong_driver_plat_data->gpios[BALONG_POWER_IND].gpio
#define GPIO_RESET_IND          	balong_driver_plat_data->gpios[BALONG_RESET_IND].gpio
#define GPIO_SUSPEND_REQUEST       balong_driver_plat_data->gpios[BALONG_SUSPEND_REQUEST].gpio
#define GPIO_SLAVE_ACTIVE       balong_driver_plat_data->gpios[BALONG_SLAVE_ACTIVE].gpio
#define BALONG_NETLINK         21
//Rename modem indication GPIO
#define GPIO_CP_PM_INDICATION    GPIO_RESET_IND     //AP: GPIO_098_M_RST_STATE; CP:RESET_N
#define GPIO_CP_SW_INDICATION    GPIO_SLAVE_ACTIVE  //AP: GPIO_062_MDM_ACTIVE; CP: GPIO_4_16;
static struct sock *balong_nlfd = NULL;
DEFINE_MUTEX(receive_sem);
static unsigned int balong_rild_pid = 0;

struct balong_nl_packet_msg {
	int balong_event;
};

typedef enum Balong_KnlMsgType {
    NETLINK_BALONG_REG = 0,    /* send from rild to register the PID for netlink kernel socket */
    NETLINK_BALONG_KER_MSG,    /* send from balong to rild */
    NETLINK_BALONG_UNREG       /* when rild exit send this type message to unregister */
} BALONG_MSG_TYPE_EN;

enum _BALONG_STATE_FLAG_ {
    BALONG_STATE_FREE,  /* unsolicited power off modem, free monitor */
    BALONG_STATE_OFF,
    BALONG_STATE_POWER,
    BALONG_STATE_READY,
    BALONG_STATE_INVALID,
};

#define HSIC_RESET_UNKNOWN     0
#define HSIC_RESET_DONE        4
static int hsic_reset_status = HSIC_RESET_UNKNOWN;


static void ap_notify_cp_reset_usb()
{
	gpio_set_value(GPIO_HOST_ACTIVE, 0);
 	msleep(10);
	gpio_set_value(GPIO_HOST_ACTIVE, 1);
}

static int balong_change_notify_event(int event)
{
	int ret;
	int size;
	unsigned char *old_tail;
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct balong_nl_packet_msg *packet;

	mutex_lock(&receive_sem);
	if (!balong_rild_pid || !balong_nlfd ) {
		pr_err("balong_power: %s, cannot notify event, g_rild_pid = %d \n",__func__, balong_rild_pid);
		ret = -1;
		goto end;
	}
	size = NLMSG_SPACE(sizeof(struct balong_nl_packet_msg));

	skb = alloc_skb(size, GFP_ATOMIC);
	if (!skb) {
		pr_info("balong_power: %s, alloc skb fail\n",__func__);
		ret = -1;
		goto end;
	}
	old_tail = skb->tail;

	nlh = NLMSG_PUT(skb, 0, 0, NETLINK_BALONG_KER_MSG, size-sizeof(*nlh));
	packet = NLMSG_DATA(nlh);
	memset(packet, 0, sizeof(struct balong_nl_packet_msg));

	packet->balong_event = event;

	nlh->nlmsg_len = skb->tail - old_tail;

	ret = netlink_unicast(balong_nlfd, skb, balong_rild_pid, MSG_DONTWAIT);
	goto end;

nlmsg_failure:
	ret = -1;
	if(skb)
		kfree_skb(skb);
end:
	mutex_unlock(&receive_sem);
	return ret;
}

static void kernel_balong_receive(struct sk_buff *skb)
{
	pr_info("balong_power: kernel_balong_receive +\n");
	mutex_lock(&receive_sem);

	if(skb->len >= sizeof(struct nlmsghdr)) {
		struct nlmsghdr *nlh;
		nlh = (struct nlmsghdr *)skb->data;
		if((nlh->nlmsg_len >= sizeof(struct nlmsghdr))&& (skb->len >= nlh->nlmsg_len)) {
			if(nlh->nlmsg_type == NETLINK_BALONG_REG) {
				balong_rild_pid = nlh->nlmsg_pid;
				pr_info("balong_power: %s, netlink receive reg packet %d ;\n",__func__, balong_rild_pid);
			} else if(nlh->nlmsg_type == NETLINK_BALONG_UNREG) {
				pr_info("balong_power: %s, netlink NETLINK_TIMER_UNREG ;\n",__func__ );
				balong_rild_pid = 0;
			}
		}
	}
	mutex_unlock(&receive_sem);
}

static void balong_netlink_init(void)
{
	balong_nlfd = netlink_kernel_create(&init_net,
	                               BALONG_NETLINK, 0, kernel_balong_receive, NULL, THIS_MODULE);
	if(!balong_nlfd)
		pr_info("balong_power: %s, netlink_kernel_create faile ;\n",__func__ );
	else
		pr_info("balong_power: %s, netlink_kernel_create success ;\n",__func__ );

	return;
}

static void balong_netlink_deinit(void)
{
    if(balong_nlfd && balong_nlfd->sk_socket)
    {
        sock_release(balong_nlfd->sk_socket);
        balong_nlfd = NULL;
    }
}

#define accu_udelay(usecs)   udelay(usecs)
#define accu_mdelay(msecs)   mdelay(msecs)
static int gpio_lowpower_set(enum iomux_func newmode)
{
 
 
	switch(newmode)
	{
	case NORMAL:
		iomux_block_set_work_mode(IO_MODEM_AP_CTRL_BLOCK_NAME );
		break;
	case GPIO:
		iomux_block_set_gpio_mode(IO_MODEM_AP_CTRL_BLOCK_NAME );
		break;
	case LOWPOWER:
		iomux_block_set_lowpower_mode(IO_MODEM_AP_CTRL_BLOCK_NAME);
		break;
	}	
 

	return 0;
}

static int get_ehci_connect_status(int* status)
{
	mm_segment_t oldfs;
	struct file *filp;
	char buf[4] = {0};
	int ret = 0;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	filp = filp_open(ECHI_DEVICE_FILE, O_RDWR, 0);

	if (!filp || IS_ERR(filp)) {
		pr_info("balong_power: Invalid EHCI filp=%ld\n", PTR_ERR(filp));
		ret = -EINVAL;
		set_fs(oldfs);
		return ret;
	} else {
		filp->f_op->read(filp, buf, 1, &filp->f_pos);
	}

	if (sscanf(buf, "%d", status) != 1)
	{
		pr_info("balong_power: Invalid result get from ehci\n");
		ret = -EINVAL;
	}

	filp_close(filp, NULL);
	set_fs(oldfs);

	return ret;
}

static bool is_echi_connected(void)
{
	int status = 0;
	int retry = 100;

	for (; retry > 0; retry--) {
		get_ehci_connect_status(&status);
		if (status == 1)
			return true;
		msleep(50);
	}

	return false;
}

/* Enable/disable USB host controller and HSIC phy */
static void enable_usb_hsic(bool enable)
{
	mm_segment_t oldfs;
	struct file *filp;
	
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	
	filp = filp_open(ECHI_DEVICE_FILE, O_RDWR, 0);
	pr_info("balong_power: EHCI filp=%ld\n", PTR_ERR(filp));
	
	if (!filp || IS_ERR(filp)) {
		/* Fs node not exit, register the ECHI device */
		/*
		if (enable && xmm_driver_plat_data &&
			xmm_driver_plat_data->echi_device){
			platform_device_register(xmm_driver_plat_data->echi_device);
		}
		*/
		pr_err("balong_power: failed to access ECHI\n");
	} else {
		if (enable) {
			filp->f_op->write(filp, "1", 1, &filp->f_pos);
		}
		else {
			filp->f_op->write(filp, "0", 1, &filp->f_pos);
		}
		filp_close(filp, NULL);
	}
	set_fs(oldfs);
}

static void set_usb_hsic_suspend(bool suspend)
{
	mm_segment_t oldfs;
	struct file *filp;
	
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	
	filp = filp_open(ECHI_DEVICE_FILE, O_RDWR, 0);
	
	if (!filp || IS_ERR(filp)) {
		pr_err("balong_power: Can not open EHCI file. filp=%ld\n", PTR_ERR(filp));
	} else {
		if (suspend) {
			filp->f_op->write(filp, "2", 1, &filp->f_pos);
		}
		else {
			filp->f_op->write(filp, "3", 1, &filp->f_pos);
		}
		filp_close(filp, NULL);
	}
	set_fs(oldfs);
}

static void notify_usb_resume(void)
{
	mm_segment_t oldfs;
	struct file *filp;
	
	oldfs = get_fs();
	set_fs(KERNEL_DS);

       /* k3 uses the file node of "/dev/ttyACM3",but we use the node of "/dev/ttyUSB0" */
	filp = filp_open("/dev/ttyUSB0", O_RDWR, 0);
	
	if (!filp || IS_ERR(filp)) {
		pr_err("balong_power: Can not open tty file. filp=%ld\n", PTR_ERR(filp));
	} else {
		filp_close(filp, NULL);
	}
	set_fs(oldfs);
}


/* Do CP power on */
static void balong_power_on(void)
{	
//	gpio_set_value(GPIO_CP_RESET, 1);
//	gpio_set_value(GPIO_CP_PMU_RESET, 1);
//	accu_mdelay(1);
	gpio_set_value(GPIO_CP_POWER_ON, 1);
//	accu_udelay(40);
}

/* Do CP power off */
static void balong_power_off(void)
{
	gpio_set_value(GPIO_CP_POWER_ON, 0);
//	accu_mdelay(1);
//	gpio_set_value(GPIO_CP_PMU_RESET, 0);
}

static inline void balong_power_change_state(enum balong_power_s_e state)
{
       if ((state >= BALONG_POW_S_MAX) ||( balong_curr_power_state >= BALONG_POW_S_MAX))
       {
           printk("balong_power_change_state: change state(%d) or current state(%d) is error\n", state, balong_curr_power_state);
           return;
       }
    pr_info("balong_power: Power state change: %s -> %s\n", power_state_name[balong_curr_power_state], power_state_name[state]);
    balong_curr_power_state = state;
}

static void balong_power_usb_on(struct work_struct *work)
{	
	pr_info("balong_power_usb_on\n");
	wake_up(&balong_wait_q);
}


static void balong_power_modem_on(struct work_struct *work)
{
	printk("\nbalong_power_modem_on!\n");
	gpio_set_value(GPIO_HOST_ACTIVE, 1);
    enable_usb_hsic(true);
	msleep(100);
 
	ap_notify_cp_reset_usb(); 


	if(0 != usb_enum_check())
	{
		printk("\nbalong_power_modem_on failed!\n");
	}
}


/* Check enumeration with CP's bootrom */
static int usb_enum_check(void)
{	
	int i;

	pr_info("balong_power: Start usb enum check 1\n");
	for (i=0; i<MAX_ENUM_RETRY; i++) {
    		msleep(250 + (i * 2));
		/* Check ECHI connect state to see if the enumeration will be success */
		if (readl(IO_ADDRESS(REG_BASE_USB2EHCI) + 0x58) & 1) {
			pr_info("balong_power: bootrom emun seems to be done\n");
			if (is_echi_connected()) {
				pr_info("balong_power: system emun done\n");
				balong_power_change_state(BALONG_POW_S_USB_ENUM_DONE);
				return 0;
			}
			else {
				pr_info("balong_power: connect signal detected but system emun failed\n");
				return -1;
			}
		}
		else {
			pr_info("balong_power: bootrom emun retry %d\n", i+1);
 
			ap_notify_cp_reset_usb();
 
		}	
	}

	return -1;
}

/* Check enumeration with CP's system-img */
static int usb_enum_check2(void)
{	
	int i;

	pr_info("balong_power: Start usb enum check 2\n");
	
	for (i=0; i<MAX_ENUM_RETRY; i++) {
		accu_mdelay(85 + i);
		/* Check ECHI connect state to see if the enumeration will be success */
		if (readl(IO_ADDRESS(REG_BASE_USB2EHCI) + 0x58) & 1) {
			pr_info("balong_power: system emun seems to be done\n");
			balong_power_change_state(BALONG_POW_S_USB_ENUM_DONE);
			return 0;
		}
		else {
			pr_info("balong_power: system emun emun retry %d\n", i+1);
 
			ap_notify_cp_reset_usb(); 
			 
		}	
	}

	return -1;
}


/* Check enumeration with CP's system-img */

static int usb_enum_check3(void)
{
	int i;
	int status = 0;

	pr_info("balong_power: Start usb enum check 3\n");

	for (i=0; i<MAX_ENUM_RETRY; i++) {
		accu_mdelay(30 + (i%3));
		/* Check ECHI connect state to see if the enumeration will be success */
		if ((readl(IO_ADDRESS(REG_BASE_USB2EHCI) + 0x58) & 1)) {
			pr_info("balong_power: resume emun seems to be done\n");
			if (is_echi_connected()) {
				pr_info("balong_power: resume emunum done\n");
				balong_power_change_state(BALONG_POW_S_USB_L0);
				gpio_set_value(GPIO_SLAVE_WAKEUP, 0);
				return 0;
			}
			else {
				pr_info("balong_power: connect signal detected but resume emun failed\n");
				return -1;
			}
		}
		else {
			pr_info("balong_power: resume emun retry %d\n", i+1);
 
			ap_notify_cp_reset_usb(); 

		}		
	}

	return -1;
}

/* Resume from L2 state */
static void balong_power_l2_resume(struct work_struct *work)
{
	msleep(10);
	//printk("%s,%d...\n", __func__, __LINE__);	
	notify_usb_resume();
}

/* Resume from L3 state */
static void balong_power_l3_resume(struct work_struct *work)
{
	int i;
	int waittime = 0;
	const int MAX_WAIT_TIME = 200; /*ms*/

	if (gpio_get_value(GPIO_HOST_WAKEUP)) {
		/* AP wakeup CP */
		printk("%s: ap wakeup cp...\n", __func__);
		accu_mdelay(5);
		gpio_set_value(GPIO_SLAVE_WAKEUP, 1);

		waittime = 0;
		while (waittime++ < MAX_WAIT_TIME)
			if (gpio_get_value(GPIO_HOST_WAKEUP) == 0)
				break;
			else
				accu_mdelay(1);

		if (waittime == MAX_WAIT_TIME)
			pr_err("balong_power: Wait for resume host wakeup falling timeout.\n");	
			
	} else {
		printk("%s: cp wakeup ap...\n", __func__);
	}

	enable_usb_hsic(true);
	msleep(10);

	balong_power_change_state(BALONG_POW_S_USB_L3_ENUM);
	gpio_set_value(GPIO_HOST_ACTIVE, 1);

	/* wait for host wakeup rising */
	waittime = 0;
	while (waittime++ < MAX_WAIT_TIME)
		if (gpio_get_value(GPIO_HOST_WAKEUP))
			break;
		else
			accu_mdelay(1);

	if (waittime == MAX_WAIT_TIME)
		pr_err("balong_power: Wait for resume host wakeup rising timeout.\n");	

	if (usb_enum_check3() != 0) {
		enable_usb_hsic(false);
		gpio_set_value(GPIO_SLAVE_WAKEUP, 0);
		msleep(10);
	}
	else {
		return;
	}

	pr_err("balong_power: Resume enumeration failed.\n");
	wake_unlock(&balong_wakelock);
}

static void balong_power_reset_work(struct work_struct *work)
{
	/* TODO: add pm func here */
	printk(  "%s balong_change_notify_event !!\n",__func__);
	balong_change_notify_event(BALONG_STATE_OFF);
}

static irqreturn_t modem_reset_isr(int irq, void *dev_id)
{
    struct device* dev = (struct device*)dev_id;
    int status=-1;
    status = gpio_get_value(GPIO_RESET_IND);
    dev_info(dev, "modem_reset_isr get. %d !!\n",status);

    dev_info(dev, "modem reset abnorm, notice ril.\n");
    queue_work(workqueue, &modem_reset_work);

    return IRQ_HANDLED;
}
 


static void balong_modem_ready_work(struct work_struct *work)
{
    if( GPIO_CP_PM_RDY==gpio_get_value(GPIO_CP_PM_INDICATION) ) {
        if( GPIO_CP_SW_RDY==gpio_get_value(GPIO_CP_SW_INDICATION) )
            balong_change_notify_event(BALONG_STATE_READY);
    }
}
/*
* GPIO_CP_SW_INDICATION hard irq
*/
static irqreturn_t mdm_sw_state_isr(int irq, void *dev_id)
{
    struct device *dev = (struct device*)dev_id;
    dev_info(dev, "GPIO_MODEM_READY_IND %s\n", __func__);

    if(GPIO_CP_SW_RDY == gpio_get_value(GPIO_CP_SW_INDICATION))
        schedule_work(&modem_ready_work);

    return IRQ_HANDLED;
}

static void request_reset_isr(struct device *dev)
{
    int status = 0;
    u32 uGPIORegsAddress;
    struct balong_power_plat_data *plat = dev->platform_data;
    int offset =  GPIO_RESET_IND % 8;
    u32 gpioic;
    u32 reset_reg_value;

    printk("%s: reset_isr_register=%d\n", __func__, reset_isr_register);

	if (reset_isr_register == 1)
		return;

	printk("%s, %d\n", __func__, __LINE__);

    if(plat && plat->reset_reg) {
        uGPIORegsAddress = IO_ADDRESS(plat->reset_reg->base_addr);
        gpioic = plat->reset_reg->gpioic;

        reset_reg_value = readb(uGPIORegsAddress + gpioic);
        reset_reg_value |= 1 << offset;
        writeb(reset_reg_value, uGPIORegsAddress + gpioic);
    }

    gpio_direction_input(GPIO_RESET_IND);

    /* Register IRQ for MODEM_RESET gpio */
    status = request_irq(gpio_to_irq(GPIO_RESET_IND),
                         modem_reset_isr,
                         IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
                         "MODEM_RESET_IRQ",
                         dev);
    if (status) {
        reset_isr_register = 0;
        dev_err(dev, "Request irq for modem reset failed\n");
    }else {
        reset_isr_register = 1;
        irq_set_irq_wake(gpio_to_irq(GPIO_RESET_IND), true);
    }
}

static void free_reset_isr(struct device *dev)
{
	printk("%s,%d: reset_isr_register=%d\n", reset_isr_register);

	if (reset_isr_register == 1)
		free_irq(gpio_to_irq(GPIO_RESET_IND),dev);

	reset_isr_register = 0;
}

static ssize_t balong_power_get(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf) 
{
	ssize_t ret = balong_curr_power_state;

	printk("~~~~~~~~~~~~~~~~~~~~~~ ret=%d\n", ret);
	return sprintf(buf, "%d\n", ret);
}
static void balong_power_usb_on_2(void)
{
    gpio_set_value(GPIO_HOST_ACTIVE, 1);
	enable_usb_hsic(true);
	msleep(usb_on_delay);
	
	/* Pull down HOST_ACTIVE to reset CP's USB */
 
	ap_notify_cp_reset_usb(); 

}

static ssize_t balong_power_set(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count) 
{
	int state;
	int i;
	unsigned long flags;	
	struct balong_power_plat_data *plat = dev->platform_data;

	dev_info(dev, "Power PHY set to %s\n", buf);

	if (sscanf(buf, "%d", &state) != 1)
		return -EINVAL;

	if (state == POWER_SET_ON) {
        unsigned long tio = msecs_to_jiffies(MAX_WAIT_TIMEOUT);
		free_reset_isr(dev);	
 
		balong_power_change_state(BALONG_POW_S_WAIT_READY);
 
		gpio_set_value(GPIO_HOST_ACTIVE, 0);
        msleep(20);
		gpio_lowpower_set(NORMAL);
		balong_power_on();
                
		tio = wait_event_timeout(balong_wait_q,balong_curr_power_state == BALONG_POW_S_USB_L0,tio);
		if (balong_curr_power_state != BALONG_POW_S_USB_L0) {
			balong_power_change_state(BALONG_POW_S_OFF);
			enable_usb_hsic(false);
			gpio_set_value(GPIO_HOST_ACTIVE, 0);
			gpio_set_value(GPIO_SLAVE_WAKEUP, 0);
			balong_power_off();
			dev_err(dev, "Wait modem system on timeout\n");
		}
		else{
			wake_lock(&balong_wakelock);			
			request_reset_isr(dev);		
			dev_err(dev, "modem usb is ok!\n");
		}
	}
	else if (state == POWER_SET_OFF){
		free_reset_isr(dev);		
		spin_lock_irqsave(&balong_state_lock, flags); 
		if (wake_lock_active(&balong_wakelock))
			wake_unlock(&balong_wakelock);
		balong_power_change_state(BALONG_POW_S_OFF);
		spin_unlock_irqrestore(&balong_state_lock, flags); 
		enable_usb_hsic(false);
		gpio_set_value(GPIO_HOST_ACTIVE, 0);
		gpio_set_value(GPIO_SLAVE_WAKEUP, 0);
		gpio_lowpower_set(LOWPOWER);
	//	balong_power_off();
	}
	else if (state == POWER_SET_DEBUGON){
		free_reset_isr(dev);		
        msleep(20);
		balong_power_on();
	    gpio_lowpower_set(NORMAL);
	}
	else if (state == POWER_SET_DEBUGOFF){
		free_reset_isr(dev);
		balong_power_off();
	    gpio_lowpower_set(LOWPOWER);
	}   
    else if (state == POWER_SET_HSIC_RESET){
		hsic_reset_status = HSIC_RESET_DONE ;
		unsigned long tio = msecs_to_jiffies(MAX_WAIT_TIMEOUT);
		spin_lock_irqsave(&balong_state_lock, flags);
		balong_power_change_state(BALONG_POW_S_WAIT_READY);
		spin_unlock_irqrestore(&balong_state_lock, flags);
		enable_usb_hsic(false);
		gpio_set_value(GPIO_HOST_ACTIVE, 0);
		tio = wait_event_timeout(balong_wait_q,
			(balong_curr_power_state == BALONG_POW_S_INIT_USB_ON),
			tio);
		if (tio == 0) {
			dev_err(dev, "Wait modem system on timeout\n");
			return -1;
		}
		
		for (i=0; i<MAX_HSIC_RESET_RETRY; i++) {
			balong_power_usb_on_2();
			if (usb_enum_check2() == 0)
				break;
			
			enable_usb_hsic(false);
			msleep(10);
		}
		
		if (i == MAX_HSIC_RESET_RETRY) {
			/* Failed to enumarte, make caller to know */
			dev_err(dev, "system enum retry failed\n");
			return 0;
		}
		
		request_reset_isr(dev);
    }
#if 1 /* Command for debug use */
	else if (state == 5){ /* L0 to L2 */
        spin_lock_irqsave(&balong_state_lock, flags);
		balong_power_change_state(BALONG_POW_S_USB_L2);
        spin_unlock_irqrestore(&balong_state_lock, flags);
		set_usb_hsic_suspend(true);
	}
	else if (state == 6){ /* L2 to L0 */
		gpio_set_value(GPIO_SLAVE_WAKEUP, 1);
	}
	else if (state == 7){ /* to L3 */
		enable_usb_hsic(false);
		gpio_set_value(GPIO_HOST_ACTIVE, 0);
        spin_lock_irqsave(&balong_state_lock, flags);
		balong_power_change_state(BALONG_POW_S_USB_L3);
        spin_unlock_irqrestore(&balong_state_lock, flags);
	}
	else if (state == 8){ /* L3 to L0 */
		queue_work(workqueue, &l3_resume_work);
	}
	else if (state == 9){
		pr_info("0\n");
		accu_mdelay(1);
		pr_info("1\n");
		accu_mdelay(2);
		pr_info("2\n");
		accu_mdelay(3);
		pr_info("3\n");
		accu_mdelay(4);
		pr_info("4\n");
		accu_mdelay(5);
		pr_info("5\n");
		accu_mdelay(10);
		pr_info("10\n");
		accu_mdelay(20);
		pr_info("20\n");
	}
#endif

	return count;
}

static ssize_t balong_gpio_set(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count) 
{
	char name[128] = {0};
	int val;
	unsigned gpio;
	int i;

	if (!balong_driver_plat_data)
		return -EINVAL;

	if (sscanf(buf, "%s %d", name, &val) != 2)
		return -EINVAL;

	for (i=0; i<ARRAY_SIZE(balong_driver_plat_data->gpios); i++)
		if (strcmp(name, balong_driver_plat_data->gpios[i].label) == 0)
			break;

	if (i == ARRAY_SIZE(balong_driver_plat_data->gpios))
		return -EINVAL;
	else
		gpio = balong_driver_plat_data->gpios[i].gpio;
	
	dev_info(dev, "%s set to %d\n", name, val);

	gpio_set_value(gpio, val);
	
	return count;
}

static ssize_t balong_gpio_get(struct device *dev,
			     struct device_attribute *attr,
			     char *buf) 
{
	char* str = buf;
	int i;

	if (!balong_driver_plat_data)
		return 0;

	for (i=0; i<ARRAY_SIZE(balong_driver_plat_data->gpios); i++)
		str += sprintf(str, "%s %d\n", balong_driver_plat_data->gpios[i].label, 
				gpio_get_value(balong_driver_plat_data->gpios[i].gpio));
	
	return str - buf;
}

static ssize_t balong_delay_set(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count) 
{
	int delay1;
	int delay2;

	if (sscanf(buf, "%d %d", &delay1, &delay2) != 2) {
		dev_err(dev, "Invalid input\n");
		return -EINVAL;
	}

	if (delay1 < 0 || delay1 > MAX_DELAY_VALUE || delay2 < 0 || delay2 > MAX_DELAY_VALUE) {
		dev_err(dev, "Invalid delay value input\n");
		return -EINVAL;
	}

	modem_on_delay = delay1;
	usb_on_delay = delay2;
	dev_info(dev, "modem_on_delay: %d, usb_on_delay: %d\n", modem_on_delay, usb_on_delay);
	
	return count;
}

/*------------------------------ SYSFS "modem_state" ------------------------------*/
static ssize_t modem_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int mdm_state;

    if( GPIO_CP_PM_RDY==gpio_get_value(GPIO_CP_PM_INDICATION) ) {
        if( GPIO_CP_SW_RDY==gpio_get_value(GPIO_CP_SW_INDICATION) )
            mdm_state = MODEM_STATE_READY;
        else
            mdm_state = MODEM_STATE_POWER;
    } else
        mdm_state = MODEM_STATE_OFF;

    return ( snprintf(buf,PAGE_SIZE,"%s\n", modem_state_str[mdm_state]) );
}

static DEVICE_ATTR(state, S_IRUGO | S_IWUSR, balong_power_get, balong_power_set);
static DEVICE_ATTR(modem_state, S_IRUGO, modem_state_show, NULL);
/* gpio and delay are for debug */
static DEVICE_ATTR(gpio, S_IRUGO | S_IWUSR, balong_gpio_get, balong_gpio_set);
static DEVICE_ATTR(delay, S_IRUGO | S_IWUSR, NULL, balong_delay_set);

static irqreturn_t host_wakeup_isr(int irq, void *dev_id)
{
	struct device* dev = (struct device*)dev_id;
	int val = gpio_get_value(GPIO_HOST_WAKEUP);
	unsigned long flags;
	if (val) {
		irq_set_irq_type(irq, IRQF_TRIGGER_LOW);
		dev_dbg(dev, "Host wakeup rising[%d].\n",balong_curr_power_state);
	}
	else {
		irq_set_irq_type(irq, IRQF_TRIGGER_HIGH);
		dev_dbg(dev, "Host wakeup falling[%d].\n",balong_curr_power_state);
	}
	spin_lock_irqsave(&balong_state_lock, flags); 
	switch (balong_curr_power_state) {
	case BALONG_POW_S_WAIT_READY:
		if (val) {
			balong_power_change_state(BALONG_POW_S_INIT_USB_ON);
			queue_work(workqueue, &modem_on_work);
		}else
		{
			if(HSIC_RESET_DONE == hsic_reset_status)
			{
				balong_power_change_state(BALONG_POW_S_INIT_USB_ON);
				wake_up(&balong_wait_q);
				hsic_reset_status = HSIC_RESET_UNKNOWN;
			}
		
		}
		break;

	case BALONG_POW_S_USB_ENUM_DONE:			
		if (val) {
			balong_power_change_state(BALONG_POW_S_USB_L0);
			queue_work(workqueue, &usb_on_work);
		}
		break;

	case BALONG_POW_S_USB_L2:
		if (!val) {
			balong_power_change_state(BALONG_POW_S_USB_L2_TO_L0);
			wake_lock(&balong_wakelock);

			if (gpio_get_value(GPIO_SLAVE_WAKEUP) == 0) {
				/* CP wakeup AP, must do something to make USB host know it */
				queue_work(workqueue, &l2_resume_work);
			}
		}
		break;

	case BALONG_POW_S_USB_L2_TO_L0:
		if (val) {
			balong_power_change_state(BALONG_POW_S_USB_L0);
			/* If AP wakeup CP, set SLAVE_WAKEUP to low to complete */
			if (gpio_get_value(GPIO_SLAVE_WAKEUP))
				gpio_set_value(GPIO_SLAVE_WAKEUP, 0);
		}
		break;
	case BALONG_POW_S_USB_L3:
		if (!val && !wake_lock_active(&balong_wakelock))
			/* Wakeup by CP, wakelock the system */
			wake_lock(&balong_wakelock);
		break;
	case BALONG_POW_S_USB_L0:
		if (!val) {
			dev_notice(dev, "Receive HOST_WAKEUP at L0, workaround\n");
		}
		break;		
	default:
		break;
	}
	spin_unlock_irqrestore(&balong_state_lock, flags); 
	return IRQ_HANDLED;
}

static int balong_power_probe(struct platform_device *pdev) 
{
    int status = -1;

    dev_info(&pdev->dev, "balong_power_probe\n");


	balong_driver_plat_data = pdev->dev.platform_data;

	iomux_block_set_work_mode(IO_MODEM_AP_CTRL_BLOCK_NAME);


	if (balong_driver_plat_data) {
		status = gpio_request_array(balong_driver_plat_data->gpios, BALONG_GPIO_NUM);
		if (status) {
			dev_err(&pdev->dev, "GPIO request failed.\n");
			goto error;
		}
	}

 


	/* Register IRQ for HOST_WAKEUP gpio */
	status = request_irq(gpio_to_irq(GPIO_HOST_WAKEUP),
    			     host_wakeup_isr,
                     IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND,
    			     "HOST_WAKEUP_IRQ",
    			     &pdev->dev);
	if (status) {
		dev_err(&pdev->dev, "Request irq for host wakeup\n");
		goto error;
    }
  

    //Modem state GPIO indication
    //GPIO_CP_SW_INDICATION
    gpio_direction_input(GPIO_CP_SW_INDICATION);
    /* GPIO_CP_SW_INDICATION interrupt registration */
    if (request_irq(gpio_to_irq(GPIO_CP_SW_INDICATION), mdm_sw_state_isr,
            IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND, "mdm_sw_state_isr", pdev) < 0) {
        dev_err(&pdev->dev, "MODEM# ERROR requesting GPIO_CP_SW_INDICATION IRQ\n");
    }

    /* sysfs entries for IO control */
    status = device_create_file(&(pdev->dev), &dev_attr_state);
    if (status) {
        dev_err(&pdev->dev, "Failed to create sysfs entry\n");
        goto error;
    }

    /* sysfs entries for modem state indication node "modem_state" */
    status = device_create_file(&(pdev->dev), &dev_attr_modem_state);
    if (status) {
        dev_err(&pdev->dev, "Failed to create sysfs entry modem_state\n");
        goto error;
    }

	status = device_create_file(&(pdev->dev), &dev_attr_gpio);
	if (status) {
		dev_err(&pdev->dev, "Failed to create sysfs entry\n");
		goto error;
	}

	status = device_create_file(&(pdev->dev), &dev_attr_delay);
	if (status) {
		dev_err(&pdev->dev, "Failed to create sysfs entry\n");
		goto error;
	}

	/* Initialize works */
	workqueue = create_singlethread_workqueue("balong_power_workqueue");
	if (!workqueue) {
		dev_err(&pdev->dev, "Create workqueue failed\n");
		status = -1;
		goto error;
	}
	INIT_WORK(&modem_on_work, balong_power_modem_on);
	INIT_WORK(&usb_on_work, balong_power_usb_on);
	INIT_WORK(&l2_resume_work, balong_power_l2_resume);
	INIT_WORK(&l3_resume_work, balong_power_l3_resume);
	INIT_WORK(&modem_reset_work, balong_power_reset_work);		
    INIT_WORK(&modem_ready_work, balong_modem_ready_work);

	init_waitqueue_head(&balong_wait_q);

	/* This wakelock will be used to arrest system sleeping when USB is in L0 state */
	wake_lock_init(&balong_wakelock, WAKE_LOCK_SUSPEND, "balong_power");
		
	balong_curr_power_state = BALONG_POW_S_OFF;

	return 0;

error:
	if (balong_driver_plat_data) {
		gpio_free_array(balong_driver_plat_data->gpios, BALONG_GPIO_NUM);
	}
	
	device_remove_file(&(pdev->dev), &dev_attr_state);
	device_remove_file(&(pdev->dev), &dev_attr_gpio);
	device_remove_file(&(pdev->dev), &dev_attr_delay);
	
	return status;
}

static int balong_power_remove(struct platform_device *pdev)
{	
	dev_dbg(&pdev->dev, "balong_power_remove\n");

	wake_lock_destroy(&balong_wakelock);

	destroy_workqueue(workqueue);
	
	device_remove_file(&(pdev->dev), &dev_attr_state);
	device_remove_file(&(pdev->dev), &dev_attr_gpio);
	device_remove_file(&(pdev->dev), &dev_attr_delay);

	if (balong_driver_plat_data) {
		gpio_free_array(balong_driver_plat_data->gpios, BALONG_GPIO_NUM);
	}

	return 0;
}

int balong_power_suspend(struct platform_device *pdev, pm_message_t state)	
{
	unsigned long flags;

	dev_info(&pdev->dev, "suspend+\n");

	spin_lock_irqsave(&balong_state_lock, flags); 
	if (balong_curr_power_state == BALONG_POW_S_USB_L2) {
		balong_power_change_state(BALONG_POW_S_USB_L3);
		spin_unlock_irqrestore(&balong_state_lock, flags); 
		enable_usb_hsic(false);
		gpio_set_value(GPIO_HOST_ACTIVE, 0);
	}
	else if (balong_curr_power_state == BALONG_POW_S_USB_L3){
		spin_unlock_irqrestore(&balong_state_lock, flags); 
		dev_warn(&pdev->dev, "already in L3 state\n");
	}
	else {
		spin_unlock_irqrestore(&balong_state_lock, flags); 
		if (balong_curr_power_state != BALONG_POW_S_OFF) {
			dev_warn(&pdev->dev, "suspend at unexpected state %d\n", balong_curr_power_state);
			return -1;
		}
	}

	dev_info(&pdev->dev, "suspend-\n");

	return 0;
}

int balong_power_resume(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "resume+\n");
	if (balong_curr_power_state == BALONG_POW_S_USB_L3) {
		if (!wake_lock_active(&balong_wakelock))
			wake_lock(&balong_wakelock);
		queue_work(workqueue, &l3_resume_work);
	}
	else if (balong_curr_power_state != BALONG_POW_S_OFF) {
		dev_info(&pdev->dev, "resume at unexpected state %d\n", balong_curr_power_state);
	}
	dev_info(&pdev->dev, "resume-\n");

	return 0;
}
static void modem_boot_shutdown(struct platform_device *pdev)
{
    dev_dbg(&pdev->dev, "modem_boot_shutdown\n");

    dev_info(&pdev->dev, "%s. Call modem_boot_set(&pdev->dev,NULL,\"2\",1) now\n", __func__);
    if( balong_power_set(&pdev->dev,NULL,"2",1)!=1 ) {    //power off modem at the init time;
        dev_err(&pdev->dev, "Failed to call modem_boot_set(&pdev->dev,NULL,\"2\",1)\n");
    }
}

static struct platform_driver balong_power_driver = {
	.probe = balong_power_probe,
	.remove = balong_power_remove,
    .shutdown = modem_boot_shutdown,
	.suspend = balong_power_suspend,
	.resume = balong_power_resume,
	.driver = {
		.name ="balong_power",
		.owner = THIS_MODULE,
	},
};

void balong_power_runtime_idle()
{
       unsigned long flags;
       if (balong_curr_power_state == BALONG_POW_S_USB_L0)
       {
			spin_lock_irqsave(&balong_state_lock, flags);
			balong_power_change_state(BALONG_POW_S_USB_L2);
			wake_unlock(&balong_wakelock);
			balong_last_suspend = jiffies;	
			spin_unlock_irqrestore(&balong_state_lock, flags);
       }
}
EXPORT_SYMBOL_GPL(balong_power_runtime_idle);

void balong_power_runtime_resume()
{
	pr_info("balong_power: bus resume\n");
	if (balong_curr_power_state == BALONG_POW_S_USB_L2
	|| balong_curr_power_state == BALONG_POW_S_USB_L2_TO_L0) {
		if (gpio_get_value(GPIO_HOST_WAKEUP)) {
			/* It's AP wakeup CP */
			unsigned long least_time = balong_last_suspend + msecs_to_jiffies(100);
			unsigned long tio;

			/* If resume just after suspending, the modem may fail
			   to resume, check this case and delay for awhile */
			while (time_before_eq(jiffies, least_time)) {
				pr_info("balong_power: suspending delayed\n");
				msleep(10);
			}

			gpio_set_value(GPIO_SLAVE_WAKEUP, 1);
			tio = jiffies + msecs_to_jiffies(200);
			while (time_before_eq(jiffies, tio)) {
				msleep(1);
				if (gpio_get_value(GPIO_HOST_WAKEUP) == 0)
					break;
			}

			if (gpio_get_value(GPIO_HOST_WAKEUP) != 0)
				pr_err("balong_power: Wait for resume USB timeout.\n");
		}
	}
	else {
		pr_err("balong_power: Invalid state %d for runtime resume.\n",
			balong_curr_power_state);
	}
}
EXPORT_SYMBOL_GPL(balong_power_runtime_resume);

static int __init balong_power_init(void)
{
	platform_driver_register(&balong_power_driver);
	spin_lock_init(&balong_state_lock);
	balong_netlink_init();		
	return 0;
}

static void __exit balong_power_exit(void)
{
	platform_driver_unregister(&balong_power_driver);
	balong_netlink_deinit();	
}

module_init(balong_power_init);
module_exit(balong_power_exit);

