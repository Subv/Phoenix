

extern "C"
{
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include "audio.h"


Audio::Audio(QObject *parent)
            : QObject(parent),
              isRunning(false),
              aout(nullptr),
              aio(nullptr),
              timer(this),
              swresample(nullptr)
{
        this->moveToThread(&thread);
        connect(&thread, SIGNAL(started()), SLOT(threadStarted()));
        thread.setObjectName("phoenix-audio");

        deviation = 0.005;

        m_abuf = new AudioBuffer();
        Q_CHECK_PTR(m_abuf);
        //connect(m_abuf, SIGNAL(hasPeriodSize()), this, SLOT(handleHasPeriodSize()));

        timer.moveToThread(&thread);
        connect(&timer, SIGNAL(timeout()), this, SLOT(handlePeriodTimer()));

        // we need send this signal to ourselves
        connect(this, SIGNAL(formatChanged()), this, SLOT(handleFormatChanged()));
}

void Audio::start()
{
    thread.start(QThread::HighestPriority);
}

/* This needs to be called on the audio thread*/
void Audio::setFormat(QAudioFormat _afmt)
{
    qCDebug(phxAudio, "setFormat(%iHz %ibits)", _afmt.sampleRate(), _afmt.sampleSize());
    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());

    afmt_in = _afmt;
    afmt_out = info.nearestFormat(_afmt); // try using the nearest supported format
    if (afmt_out.sampleRate() < afmt_in.sampleRate()) {
        // if that got us a format with a worse sample rate, use preferred format
        afmt_out = info.preferredFormat();
    }
    qCDebug(phxAudio, "Using nearest format supported by sound card: %iHz %ibits",
                      afmt_out.sampleRate(), afmt_out.sampleSize());

    swresample = swr_alloc_set_opts(swresample,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, afmt_out.sampleRate(),
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, afmt_in.sampleRate(),
            0, nullptr
    );
    swr_init(swresample);
    emit formatChanged();
}

void Audio::handleFormatChanged()
{
    if (aout) {
        aout->stop();
        delete aout;
    }
    aout = new QAudioOutput(afmt_out);
    Q_CHECK_PTR(aout);
    aout->moveToThread(&thread);

    connect(aout, SIGNAL(stateChanged(QAudio::State)), SLOT(stateChanged(QAudio::State)));
    aio = aout->start();
    if (!isRunning)
        aout->suspend();

    timer.setInterval(afmt_out.durationForBytes(aout->periodSize() * 1.5) / 1000);
    aio->moveToThread(&thread);
}

void Audio::threadStarted()
{
    if(!afmt_in.isValid()) {
        // we don't have a valid audio format yet...
        return;
    }
    handleFormatChanged();
}

void Audio::handlePeriodTimer()
{
    Q_ASSERT(QThread::currentThread() == &thread);
    Q_ASSERT(aio);
    int toWrite = aout->bytesFree();
    if(!toWrite)
        return;

    int half_size = aout->bufferSize() / 2;
    int delta_mid = toWrite - half_size;
    qreal direction = (qreal)delta_mid / half_size;
    qreal adjust = 1.0 + deviation * direction;

    QVarLengthArray<char, 4096*4> tmpbuf(m_abuf->size());
    int read = m_abuf->read(tmpbuf.data(), tmpbuf.size());

    uint8_t *output;
    int samplesToWrite = afmt_out.framesForBytes(toWrite);
    av_samples_alloc(&output, NULL, 2, samplesToWrite, AV_SAMPLE_FMT_S16, 0);

    const uint8_t *input[] = { (const uint8_t *)tmpbuf.data() };
    int converted = swr_convert(swresample, &output, samplesToWrite,
                                input, read / afmt_in.bytesPerFrame());
//    qCDebug(phxAudio) << converted << toWrite << aout->bufferSize();

    int wrote = aio->write((char *)output, afmt_out.bytesForFrames(converted));
    Q_UNUSED(wrote);
    av_freep(&output);
}

void Audio::runChanged(bool _isRunning)
{
    isRunning = _isRunning;
    if(!aout)
        return;
    if(!isRunning) {
        if(aout->state() != QAudio::SuspendedState) {
            qCDebug(phxAudio) << "Paused";
            aout->suspend();
            timer.stop();
        }
    } else {
        if(aout->state() != QAudio::ActiveState) {
            qCDebug(phxAudio) << "Started";
            aout->resume();
            timer.start();
        }
    }
}

void Audio::setVolume(qreal level)
{
    if (aout)
        aout->setVolume(level);
}

Audio::~Audio()
{
    if(aout)
        delete aout;
    if(aio)
        delete aio;
    if(m_abuf)
        delete m_abuf;
}
