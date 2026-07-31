#include "drivers/audio/audio_core.h"
#include "core/errno.h"
#include "sys/usercopy.h"

static int audio_format_equal(const a20_audio_format_t *a,
                              const a20_audio_format_t *b)
{
    return a->rate == b->rate && a->channels == b->channels &&
           a->format == b->format;
}

int audio_device_ioctl(device_t *dev, const audio_dev_ops_t *ops,
                       unsigned long req, void *arg)
{
    if (!dev || !ops)
        return -ENODEV;
    if (req == A20_AUDIO_IOCTL_STOP)
        return ops->stop ? ops->stop(dev) : -EOPNOTSUPP;
    if (req == A20_AUDIO_IOCTL_DRAIN)
        return ops->drain ? ops->drain(dev) : -EOPNOTSUPP;
    if (!arg)
        return -EFAULT;

    if (req == A20_AUDIO_IOCTL_GET_CAPS)
        return copy_to_user(arg, &ops->caps, sizeof(ops->caps)) < 0 ?
               -EFAULT : 0;
    if (req == A20_AUDIO_IOCTL_SET_FORMAT) {
        a20_audio_format_t format;
        if (!(ops->caps.flags & A20_AUDIO_CAP_PCM))
            return -EOPNOTSUPP;
        if (copy_from_user(&format, arg, sizeof(format)) < 0)
            return -EFAULT;
        if (!audio_format_equal(&format, &ops->pcm_format))
            return -EINVAL;
        return ops->set_format ? ops->set_format(dev, &format) : 0;
    }
    if (req == A20_AUDIO_IOCTL_TONE) {
        a20_audio_tone_t tone;
        if (!(ops->caps.flags & A20_AUDIO_CAP_TONE) || !ops->tone)
            return -EOPNOTSUPP;
        if (copy_from_user(&tone, arg, sizeof(tone)) < 0)
            return -EFAULT;
        if (tone.frequency_hz < ops->caps.min_rate ||
            tone.frequency_hz > ops->caps.max_rate)
            return -EINVAL;
        return ops->tone(dev, &tone);
    }
    return ops->ioctl ? ops->ioctl(dev, req, arg) : -ENOTTY;
}

int audio_device_close(device_t *dev, const audio_dev_ops_t *ops)
{
    if (!dev || !ops)
        return -ENODEV;

    int ret = 0;
    if ((ops->caps.flags & A20_AUDIO_CAP_PCM) && ops->drain) {
        ret = ops->drain(dev);
        if (ret < 0 && ops->stop) {
            int stop_ret = ops->stop(dev);
            if (stop_ret < 0)
                ret = stop_ret;
        }
    } else if ((ops->caps.flags & A20_AUDIO_CAP_TONE) && ops->stop) {
        ret = ops->stop(dev);
    }
    if (ops->close) {
        int close_ret = ops->close(dev);
        if (ret == 0)
            ret = close_ret;
    }
    return ret;
}
