#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>
#include <sound/simple_card.h>
#include "greybus.h"
#include "i2s.h"

#define GB_AUDIO_DATA_DRIVER_NAME			"gb_audio_data"
#define GB_AUDIO_MGMT_DRIVER_NAME			"gb_audio_mgmt"

#define GB_RATES				SNDRV_PCM_RATE_8000_48000
#define GB_FMTS					SNDRV_PCM_FMTBIT_S16_LE
#define GB_MAX_LENGTH				256L
#define PREALLOC_BUFFER				(32 * 1024)
#define PREALLOC_BUFFER_MAX			(32 * 1024)


#define CONFIG_SAMPLES_PER_MSG         48  /* assuming 1 ms samples @ 48KHz */
#define CONFIG_PERIOD_NS               1000000 /* send msg every 1ms */

#define CONFIG_COUNT_MAX               32
#define CONFIG_I2S_REMOTE_DATA_CPORT   4 /* XXX this shouldn't be hardcoded...*/

/* Switch between dummy spdif and jetson rt5645 codec */
#define USE_RT5645 0



/***********************************
 * GB I2S helper functions
 ***********************************/
static int gb_i2s_mgmt_activate_cport(struct gb_connection *connection,
					uint16_t cport)
{
	struct gb_i2s_mgmt_activate_cport_request request;

	request.cport = cport;

	return gb_operation_sync(connection, GB_I2S_MGMT_TYPE_ACTIVATE_CPORT,
				&request, sizeof(request), NULL, 0);
}

static int gb_i2s_mgmt_deactivate_cport(struct gb_connection *connection,
					uint16_t cport)
{
	struct gb_i2s_mgmt_deactivate_cport_request request;

	request.cport = cport;

	return gb_operation_sync(connection, GB_I2S_MGMT_TYPE_DEACTIVATE_CPORT,
				&request, sizeof(request), NULL, 0);
}

static int gb_i2s_mgmt_get_supported_configurations(
			struct gb_connection *connection,
			struct gb_i2s_mgmt_get_supported_configurations_response *get_cfg,
			size_t size)
{
	return gb_operation_sync(connection, GB_I2S_MGMT_TYPE_GET_SUPPORTED_CONFIGURATIONS,
					NULL, 0, get_cfg, size);
}

static int gb_i2s_mgmt_set_supported_configuration(
			struct gb_connection *connection,
			struct gb_i2s_mgmt_set_configuration_request *set_cfg)
{
	return gb_operation_sync(connection,
					GB_I2S_MGMT_TYPE_SET_CONFIGURATION,
					set_cfg, sizeof(*set_cfg), NULL, 0);
}

static int gb_i2s_mgmt_set_configuration(struct gb_connection *connection,
				struct gb_i2s_mgmt_set_configuration_request *set_cfg)
{
	return gb_operation_sync(connection, GB_I2S_MGMT_TYPE_SET_CONFIGURATION,
					set_cfg, sizeof(*set_cfg), NULL, 0);
}

static int gb_i2s_mgmt_set_samples_per_message(
				struct gb_connection *connection,
				uint16_t samples_per_message)
{
	struct gb_i2s_mgmt_set_samples_per_message_request request;
	request.samples_per_message = samples_per_message;

	return gb_operation_sync(connection,
				GB_I2S_MGMT_TYPE_SET_SAMPLES_PER_MESSAGE,
				&request, sizeof(request), NULL, 0);
}

static int gb_i2s_mgmt_setup(struct gb_connection *connection)
{
	struct gb_i2s_mgmt_get_supported_configurations_response *get_cfg;
	struct gb_i2s_mgmt_set_configuration_request set_cfg;
	struct gb_i2s_mgmt_configuration *cfg;
	size_t size;
	int i, ret;

	size = sizeof(*get_cfg) + (CONFIG_COUNT_MAX * sizeof(get_cfg->config[0]));

	get_cfg = kzalloc(size, GFP_KERNEL);
	if (!get_cfg) {
		pr_err("get_cfg alloc failed\n");
		return -ENOMEM;
	}
#if 0
	ret = gb_i2s_mgmt_get_supported_configurations(connection, get_cfg, size);
	if (ret) {
		pr_err("get_supported_config failed: %d\n", ret);
		goto free_get_cfg;
	}

	/* Pick 48KHz 16-bits/channel */
	for (i = 0, cfg = get_cfg->config; i < CONFIG_COUNT_MAX; i++, cfg++) {
		if ((cfg->sample_frequency == 48000) &&
				(cfg->num_channels == 2) &&
				(cfg->bytes_per_channel == 2) &&
				(cfg->byte_order & GB_I2S_MGMT_BYTE_ORDER_LE) &&
				(cfg->spatial_locations ==
					(GB_I2S_MGMT_SPATIAL_LOCATION_FL |
					GB_I2S_MGMT_SPATIAL_LOCATION_FR)) &&
				(cfg->ll_protocol & GB_I2S_MGMT_PROTOCOL_I2S) &&
				(cfg->ll_bclk_role & GB_I2S_MGMT_ROLE_MASTER) &&
				(cfg->ll_wclk_role & GB_I2S_MGMT_ROLE_MASTER) &&
				(cfg->ll_wclk_polarity & GB_I2S_MGMT_POLARITY_NORMAL) &&
				(cfg->ll_wclk_polarity & GB_I2S_MGMT_POLARITY_NORMAL) &&
				(cfg->ll_wclk_change_edge & GB_I2S_MGMT_EDGE_FALLING) &&
				(cfg->ll_wclk_tx_edge & GB_I2S_MGMT_EDGE_FALLING) &&
				(cfg->ll_wclk_rx_edge & GB_I2S_MGMT_EDGE_RISING) &&
				(cfg->ll_data_offset == 1))
			break;
	}

	if (i >= CONFIG_COUNT_MAX) {
		pr_err("No valid configuration\n");
		ret = -EINVAL;
		goto free_get_cfg;
	}

	memcpy(&set_cfg, cfg, sizeof(set_cfg));
#endif
	set_cfg.config.byte_order = GB_I2S_MGMT_BYTE_ORDER_LE;
	set_cfg.config.ll_protocol = GB_I2S_MGMT_PROTOCOL_I2S;
	set_cfg.config.ll_bclk_role = GB_I2S_MGMT_ROLE_MASTER;
	set_cfg.config.ll_wclk_role = GB_I2S_MGMT_ROLE_MASTER;
	set_cfg.config.ll_wclk_polarity = GB_I2S_MGMT_POLARITY_NORMAL;
	set_cfg.config.ll_wclk_change_edge = GB_I2S_MGMT_EDGE_RISING;
	set_cfg.config.ll_wclk_tx_edge = GB_I2S_MGMT_EDGE_FALLING;
	set_cfg.config.ll_wclk_rx_edge = GB_I2S_MGMT_EDGE_RISING;
	ret = gb_i2s_mgmt_set_configuration(connection, &set_cfg);
	if (ret) {
		pr_err("set_supported_config failed: %d\n", ret);
		goto free_get_cfg;
	}

	ret = gb_i2s_mgmt_set_samples_per_message(connection,
						CONFIG_SAMPLES_PER_MSG);
	if (ret) {
		pr_err("set_samples_per_msg failed: %d\n", ret);
		goto free_get_cfg;
	}

	/* XXX Add start delay here (probably 1ms) */
	ret = gb_i2s_mgmt_activate_cport(connection,
						CONFIG_I2S_REMOTE_DATA_CPORT);
	if (ret) {
		pr_err("activate_cport failed: %d\n", ret);
		goto free_get_cfg;
	}

free_get_cfg:
	kfree(get_cfg);
	return ret;
}

/***************************************************************
 * This is the gb_snd structure which ties everything together
 * and fakes DMA interrupts via a timer. Also the gb_snd_list
 * mgmt logic is here.
 ***************************************************************/
struct gb_snd {
	struct platform_device		*card;
	struct platform_device		*cpu_dai;
	struct gb_connection		*mgmt_connection;
	struct gb_connection		*i2s_tx_connection;
	struct gb_connection		*i2s_rx_connection;
	int				gb_bundle_id;
	int				device_count;
	struct snd_pcm_substream	*substream;
	struct hrtimer			timer;
	atomic_t			running;
	struct workqueue_struct		*workqueue;
	struct work_struct		work;
	int				hwptr_done;
	struct list_head		list;
};


/* XXX needs a lock! */
static LIST_HEAD(gb_snd_list);
int device_count;

static struct gb_snd *gb_find_snd(int bundle_id)
{
	struct gb_snd *tmp;
	list_for_each_entry(tmp, &gb_snd_list, list)
		if (tmp->gb_bundle_id == bundle_id)
			return tmp;
	return NULL;
}

static struct gb_snd *gb_get_snd(int bundle_id)
{
	struct gb_snd *snd_dev;
	snd_dev = gb_find_snd(bundle_id);
	if(snd_dev) {
		return snd_dev;
	}

	snd_dev = kzalloc(sizeof(*snd_dev), GFP_KERNEL);
	if(!snd_dev)
		return NULL;

	snd_dev->device_count = device_count++;
	snd_dev->gb_bundle_id = bundle_id;
	list_add(&snd_dev->list, &gb_snd_list);
	return snd_dev;
}

static void gb_free_snd(struct gb_snd* snd)
{
	if (!snd->i2s_tx_connection &&
			!snd->mgmt_connection) {
		list_del(&snd->list);
		kfree(snd);
	}
}


/*********************************************************
 * timer logic
 *********************************************************/

static void snd_dev_work(struct work_struct *work)
{
	struct gb_snd *snd_dev = container_of(work, struct gb_snd, work);
	struct snd_pcm_substream *substream = snd_dev->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	char* address;
	long len;
	int ret;

	if (!snd_dev)
		return;

	if (!atomic_read(&snd_dev->running))
		 return;

	address = runtime->dma_area + snd_dev->hwptr_done;
	len = frames_to_bytes(runtime, runtime->buffer_size) - snd_dev->hwptr_done;
	len = min(len, GB_MAX_LENGTH);
	ret = gb_operation_sync(snd_dev->i2s_tx_connection, GB_I2S_DATA_TYPE_SEND_DATA,
					address, len, NULL, 0);
	snd_dev->hwptr_done += len;
	/* XXX - Probably need to call this less frequently */
	snd_pcm_period_elapsed(snd_dev->substream);
}

static enum hrtimer_restart dummy_timer_function(struct hrtimer *hrtimer)
{
	struct gb_snd *snd_dev = container_of(hrtimer, struct gb_snd, timer);

	if (!atomic_read(&snd_dev->running))
		return HRTIMER_NORESTART;
	queue_work(snd_dev->workqueue, &snd_dev->work);
	hrtimer_forward_now(hrtimer,ktime_set(0, CONFIG_PERIOD_NS));
	return HRTIMER_RESTART;
}

static void dummy_hrtimer_start(struct gb_snd *snd_dev)
{
	hrtimer_start(&snd_dev->timer, ktime_set(0, CONFIG_PERIOD_NS),
						HRTIMER_MODE_REL);
	atomic_set(&snd_dev->running, 1);

}

static void dummy_hrtimer_stop(struct gb_snd *snd_dev)
{
	atomic_set(&snd_dev->running, 0);
	hrtimer_cancel(&snd_dev->timer);
}

static int dummy_hrtimer_init(struct gb_snd *snd_dev)
{
	hrtimer_init(&snd_dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	snd_dev->timer.function = dummy_timer_function;
	atomic_set(&snd_dev->running, 0);
	snd_dev->workqueue = alloc_workqueue("gb-audio", WQ_HIGHPRI, 0);
	if (!snd_dev->workqueue)
		return -ENOMEM;
	INIT_WORK(&snd_dev->work, snd_dev_work);
	return 0;
}

/******************************************
 * dai op functions)
 ******************************************/

static int gb_dai_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	return 0;
}

static void gb_dai_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
}

static int gb_dai_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct gb_snd *snd_dev;


	snd_dev = snd_soc_dai_get_drvdata(rtd->cpu_dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	//	gb_i2s_mgmt_activate_cport(snd_dev->mgmt_connection, 1);
		dummy_hrtimer_start(snd_dev);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		dummy_hrtimer_stop(snd_dev);
	//	gb_i2s_mgmt_deactivate_cport(snd_dev->mgmt_connection, 1);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int gb_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	return 0;
}

static int gb_dai_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	return 0;
}


static const struct snd_soc_dai_ops gb_dai_ops = {
	.startup        = gb_dai_startup,
	.shutdown       = gb_dai_shutdown,
	.trigger        = gb_dai_trigger,
	.set_fmt        = gb_dai_set_fmt,
	.hw_params      = gb_dai_hw_params,
};

static struct snd_soc_dai_driver gb_cpu_dai = {
	.name                   = "gb-cpu-dai",
	.playback = {
		.rates          = GB_RATES,
		.formats        = GB_FMTS,
		.channels_min   = 2,
		.channels_max   = 2,
	},
	.ops = &gb_dai_ops,
};

/******************************************************
 * gb pcm logic
 *****************************************************/
static struct snd_pcm_hardware gb_plat_pcm_hardware = {
	.info =         SNDRV_PCM_INFO_INTERLEAVED,
	.formats                = GB_FMTS,
	.rates                  = GB_RATES,
	.rate_min               = 8000,
	.rate_max               = 48000,
	.channels_min           = 2,
	.channels_max           = 2,
	/* XXX - All the values below are junk */
	.buffer_bytes_max       = 64 * 1024,
	.period_bytes_min       = 32,
	.period_bytes_max       = 8192,
	.periods_min            = 1,
	.periods_max            = 32,
	.fifo_size              = 256,
};

static snd_pcm_uframes_t gb_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct gb_snd *snd_dev;

	snd_dev = snd_soc_dai_get_drvdata(rtd->cpu_dai);

	return snd_dev->hwptr_done;
}

static int gb_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct gb_snd *snd_dev;

	snd_dev = snd_soc_dai_get_drvdata(rtd->cpu_dai);
	snd_dev->hwptr_done = 0;

	return 0;
}

static int gb_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct gb_snd *snd_dev;
	int ret;

	snd_dev = snd_soc_dai_get_drvdata(rtd->cpu_dai);

	runtime->private_data = snd_dev;
	snd_dev->substream = substream;
	ret = dummy_hrtimer_init(snd_dev);
	if (ret)
		return ret;

	snd_soc_set_runtime_hwparams(substream, &gb_plat_pcm_hardware);
	return snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int gb_pcm_close(struct snd_pcm_substream *substream)
{
	substream->runtime->private_data = NULL;
	return 0;
}

static int gb_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *hw_params)
{
	printk("gb_pcm_hw_params\n");
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int gb_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return snd_pcm_lib_free_pages(substream);
}

static struct snd_pcm_ops gb_pcm_ops = {
	.open           = gb_pcm_open,
	.close		= gb_pcm_close,
	.ioctl          = snd_pcm_lib_ioctl,
	.hw_params      = gb_pcm_hw_params,
	.hw_free        = gb_pcm_hw_free,
	.prepare	= gb_pcm_prepare,
	.pointer        = gb_pcm_pointer,
};

static void gb_pcm_free(struct snd_pcm *pcm)
{
	snd_pcm_lib_preallocate_free_for_all(pcm);
}

static int gb_pcm_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;

	return snd_pcm_lib_preallocate_pages_for_all(
			pcm,
			SNDRV_DMA_TYPE_CONTINUOUS,
			snd_dma_continuous_data(GFP_KERNEL),
			PREALLOC_BUFFER, PREALLOC_BUFFER_MAX);
}

static struct snd_soc_platform_driver gb_soc_platform = {
	.ops            = &gb_pcm_ops,
	.pcm_new        = gb_pcm_new,
	.pcm_free       = gb_pcm_free,
};


/************************************************************
 * This is the aosc simple card junk which binds the platform
 * codec, cpu and codec-dais etc togheter, also all the
 * nested gross platfrom driver/device junk is here.
 ************************************************************/
static const struct snd_soc_component_driver gb_soc_component = {
	.name           = "gb-component",
};

static int gb_plat_probe(struct platform_device *pdev)
{
	struct gb_snd *snd_dev;
	int ret;

	snd_dev = (struct gb_snd *)pdev->dev.platform_data;
	dev_set_drvdata(&pdev->dev, snd_dev);

	ret = snd_soc_register_platform(&pdev->dev, &gb_soc_platform);
	ret = snd_soc_register_component(&pdev->dev, &gb_soc_component,
							&gb_cpu_dai, 1);
	return ret;
}

static struct platform_driver gb_plat_driver = {
	.driver         = {
		.name   = "gb-pcm-audio",
	},
	.probe          = gb_plat_probe,
};


static struct asoc_simple_card_info gb_card_info = {
	.name           = "Greybus Audio Module",
	.card           = "gb-card",
#if USE_RT5645
//	.codec          = "rt5645.0-001c", /* XXX this will need to be dynamic*/
	.codec          = "rt5645.6-001b", /* XXX this will need to be dynamic*/
	.daifmt         = GB_FMTS,
#else
	.codec          = "spdif-dit",
#endif
	.platform       = "gb-pcm-audio.0",
	.cpu_dai = {
		.name   = "gb-pcm-audio.0",
		.fmt    = GB_FMTS,
	},
	.codec_dai = {
#if USE_RT5645
		.name   = "rt5645-aif1",
		.fmt    = SND_SOC_DAIFMT_CBM_CFM,
		.sysclk = 11289600,
#else
		.name   = "dit-hifi",
#endif
	}
};



/****************************************************************
 * GB hooks
 ****************************************************************/
static int gb_i2s_transmitter_connection_init(struct gb_connection *connection)
{
	struct gb_snd *snd_dev;
	int ret;

	snd_dev = gb_get_snd(connection->bundle->id);
	if(!snd_dev)
		return -ENOMEM;

	snd_dev->cpu_dai = platform_device_alloc("gb-pcm-audio", snd_dev->device_count);
	if (!snd_dev->cpu_dai) {
		ret = ENOMEM;
		goto out;
	}

	snd_dev->card = platform_device_alloc("asoc-simple-card", snd_dev->device_count);
	if (!snd_dev->card) {
		ret = ENOMEM;
		goto out_dai;
	}

	snd_dev->i2s_tx_connection = connection;

	snd_dev->card->dev.platform_data = &gb_card_info; /*XXX - prob should generate this dynamically*/
	snd_dev->cpu_dai->dev.platform_data = snd_dev;
	snd_dev->i2s_tx_connection->private = snd_dev;

	ret = platform_device_add(snd_dev->cpu_dai);
	if (ret) {
		goto out_dai;
	}
	ret = platform_device_add(snd_dev->card);
	if (ret) {
		/* XXX errrr.. figure out the right thing here... */
		//platform_device_unregister(snd_dev->cpu_dai);
		goto out_card;
	}
	return 0;

out_card:
	platform_device_put(snd_dev->card);
out_dai:
	platform_device_put(snd_dev->cpu_dai);
out:
	gb_free_snd(snd_dev);
	return ret;
}

static void gb_i2s_transmitter_connection_exit(struct gb_connection *connection)
{
	struct gb_snd *snd_dev;

	snd_dev = (struct gb_snd *)connection->private;

	platform_device_unregister(snd_dev->card);
	platform_device_unregister(snd_dev->cpu_dai);
	snd_dev->i2s_tx_connection = NULL;
	gb_free_snd(snd_dev);
}

static int gb_i2s_mgmt_connection_init(struct gb_connection *connection)
{
	struct gb_snd *snd_dev;

	snd_dev = gb_get_snd(connection->bundle->id);
	if(!snd_dev)
		return -ENOMEM;

	snd_dev->mgmt_connection = connection;
	connection->private = snd_dev;

	gb_i2s_mgmt_setup(connection);
	return 0;
}

static void gb_i2s_mgmt_connection_exit(struct gb_connection *connection)
{
	struct gb_snd *snd_dev;

	snd_dev = (struct gb_snd *)connection->private;

	snd_dev->mgmt_connection = NULL;
	gb_free_snd(snd_dev);
}

static struct gb_protocol gb_i2s_receiver_protocol = {
	.name			= GB_AUDIO_DATA_DRIVER_NAME,
	.id			= GREYBUS_PROTOCOL_I2S_RECEIVER,
	.major			= 0,
	.minor			= 1,
	.connection_init	= gb_i2s_transmitter_connection_init,
	.connection_exit	= gb_i2s_transmitter_connection_exit,
	.request_recv		= NULL,
};

static struct gb_protocol gb_i2s_mgmt_protocol = {
	.name			= GB_AUDIO_MGMT_DRIVER_NAME,
	.id			= GREYBUS_PROTOCOL_I2S_MGMT,
	.major			= 0,
	.minor			= 1,
	.connection_init	= gb_i2s_mgmt_connection_init,
	.connection_exit	= gb_i2s_mgmt_connection_exit,
	.request_recv		= NULL,
};

/******************************************************************
 * This is the basic hook to let me get things initialized
 ******************************************************************/
static int __init devices_setup(void)
{
	int err;
	struct platform_device *device;

	err = gb_protocol_register(&gb_i2s_mgmt_protocol);
	if (err) {
		pr_err("Can't register i2s mgmt protocol driver: %d\n", -err);
		return err;
	}

	err = gb_protocol_register(&gb_i2s_receiver_protocol);
	if (err) {
		pr_err("Can't register Audio protocol driver: %d\n", -err);
		goto err_unregister_i2s_mgmt;
	}

	err = platform_driver_register(&gb_plat_driver);
	if (err) {
		pr_err("Can't register platform driver: %d\n", -err);
		goto err_unregister_audio;
	}

	device = platform_device_register_simple("spdif-dit", -1, NULL, 0);
	return 0;

err_unregister_audio:
	gb_protocol_deregister(&gb_i2s_receiver_protocol);
err_unregister_i2s_mgmt:
	gb_protocol_deregister(&gb_i2s_mgmt_protocol);
	return err;
}
device_initcall(devices_setup);
MODULE_LICENSE("GPL");
