"""Apply checked, test-only telemetry hooks to a private FreeRDP 3.32.1 source copy."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
MARKER = '// RemoteManager test-only comparator probe\n'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned SDL probe anchor changed or is ambiguous')
    return text.replace(old, new, 1)


def upload_batch(context, window):
    if 'sdlPerformance::beginUploads();' not in context:
        # Limit the replacement to drawToWindow; other useLocalScale branches remain upstream.
        start = context.find('bool SdlContext::drawToWindow(')
        end = context.find('\n}\n', start)
        if start < 0: start, end = 0, len(context)
        else: end += 3
        prefix, body, suffix = context[:start], context[start:end], context[end:]
        body = replace(body, '\tif (useLocalScale())', '\tsdlPerformance::beginUploads();\n\tif (useLocalScale())')
        body = replace(body, '\twindow.updateSurface();', '\tsdlPerformance::endUploads();\n\twindow.updateSurface();')
        context = prefix + body + suffix
    if 'sdlPerformance::uploaded(' not in window:
        window = replace(window, '\tconst auto probeBegin = PerformanceTrace::now();\n'
                         '\tif (!SDL_UpdateTexture', '\tif (!SDL_UpdateTexture')
        window = replace(window,
            '\tsdlPerformance::trace.record({PerformanceStage::UploadPreparation, sdlPerformance::stamp(),\n'
            '\t    probeBegin, PerformanceTrace::now(), quint64(srcRect.w) * quint64(srcRect.h) * quint64(bpp)});',
            '\tsdlPerformance::uploaded(quint64(srcRect.w) * quint64(srcRect.h) * quint64(bpp));')
    return context, window


def final_event_poll(text):
    if 'sdlPerformance::finalEvent, sdlPerformance::finalEvent);' in text: return text
    return replace(text,
        '\t\t\t\tconst int prc = SDL_PeepEvents(&windowEvent, 1, SDL_GETEVENT, SDL_EVENT_FIRST,\n'
        '\t\t\t\t                               SDL_EVENT_USER_RETRY_DIALOG);',
        '\t\t\t\tint prc = SDL_PeepEvents(&windowEvent, 1, SDL_GETEVENT, SDL_EVENT_FIRST,\n'
        '\t\t\t\t                               SDL_EVENT_USER_RETRY_DIALOG);\n'
        '\t\t\t\tif (prc == 0 && sdlPerformance::enabled())\n'
        '\t\t\t\t\tprc = SDL_PeepEvents(&windowEvent, 1, SDL_GETEVENT,\n'
        '\t\t\t\t\t    sdlPerformance::finalEvent, sdlPerformance::finalEvent);')


def instrument(source):
    base = source / 'client/SDL/SDL3'
    probe = base / 'remotemanager-probe'
    probe.mkdir(exist_ok=True)
    for path in (ROOT / 'app/PerformanceTrace.h', ROOT / 'app/PerformanceTrace.cpp', ROOT / 'tests/SdlPerformanceProbe.h', ROOT / 'tests/PerformanceMarker.h'):
        shutil.copyfile(path, probe / path.name)
    context = base / 'sdl_context.cpp'
    text = context.read_text()
    if not text.startswith(MARKER):
        text = replace(text, '#include "sdl_context.hpp"', '#include "sdl_context.hpp"\n#include "remotemanager-probe/SdlPerformanceProbe.h"')
        text = replace(text, 'BOOL SdlContext::preConnect(freerdp* instance)\n{',
                       'BOOL SdlContext::preConnect(freerdp* instance)\n{\n\tif (!sdlPerformance::prepare(instance->context)) return FALSE;')
        text = replace(text, 'context->update->EndPaint = endPaint;',
                       'context->update->EndPaint = endPaint;\n\tif (!sdlPerformance::install(context)) return FALSE;')
        text = replace(text, '\t_queue.emplace(std::move(rects));',
                       '\t_queue.emplace(std::move(rects));\n\t{ std::lock_guard<std::mutex> probeLock(sdlPerformance::mutex); sdlPerformance::push(); }')
        text = replace(text, '\t_queue.pop();',
                       '\t_queue.pop();\n\t{ std::lock_guard<std::mutex> probeLock(sdlPerformance::mutex); sdlPerformance::pop(); }')
        text = replace(text, '\tsdl->push(std::move(rects));',
                       '\t{ std::lock_guard<std::mutex> probeLock(sdlPerformance::mutex); sdlPerformance::capture(gdi); }\n\tsdl->push(std::move(rects));')
        text = replace(text, '\t\tif (!freerdp_check_event_handles(context()))',
                       '\t\tconst auto probeBegin = PerformanceTrace::now();\n'
                       '\t\tconst BOOL probeOk = freerdp_check_event_handles(context());\n'
                       '\t\tsdlPerformance::trace.record({PerformanceStage::Processing, {}, probeBegin, PerformanceTrace::now()});\n'
                       '\t\tif (!probeOk)')
        text = replace(text, '\twindow.updateSurface();', '\twindow.updateSurface();\n\tsdlPerformance::input(window.window());')
        context.write_text(MARKER + text)
    text = context.read_text()
    if 'sdlPerformance::prepare(instance->context))' in text:
        text = replace(text, 'sdlPerformance::prepare(instance->context))',
                       'sdlPerformance::prepare(instance->context, SDL_EVENT_USER_SCARD_RESULT))')
        context.write_text(text)
    if 'RM_COMPARATOR_JOINED' not in text:
        text = replace(text, '\t_thread.join();',
                       '\t_thread.join();\n\tif (sdlPerformance::enabled() && sdlPerformance::reportWritten) {\n'
                       '\t\tstd::printf("RM_COMPARATOR_JOINED=1\\n"); std::fflush(stdout);\n\t}')
        context.write_text(text)
    if 'sdlPerformance::cancelled' not in text:
        text = replace(text, '\t\tif (!probeOk)\n\t\t{',
                       '\t\tif (!probeOk)\n\t\t{\n\t\t\tsdlPerformance::cancelled(context());')
        context.write_text(text)
    if 'sdlPerformance::verificationReady = true' not in text:
        text = replace(text, 'bool SdlContext::handleEvent(const SDL_Event& ev)\n{',
                       'bool SdlContext::handleEvent(const SDL_Event& ev)\n{\n'
                       '\tif (sdlPerformance::enabled() && ev.type == sdlPerformance::finalEvent) {\n'
                       '\t\tsdlPerformance::verificationReady = true;\n'
                       '\t\tfor (auto& window : _windows) window.second.updateSurface();\n'
                       '\t\treturn true;\n\t}\n')
        context.write_text(text)
    client = base / 'sdl_freerdp.cpp'
    text = client.read_text()
    if 'sdlPerformance::beforeAbort' not in text:
        text = replace(text, '#include "sdl_context.hpp"',
                       '#include "sdl_context.hpp"\n#include "remotemanager-probe/SdlPerformanceProbe.h"')
        text = replace(text, '\t\t\t\t\t\tstd::ignore = freerdp_abort_connect_context(sdl->context());',
                       '\t\t\t\t\t\tsdlPerformance::beforeAbort(sdl->context());\n'
                       '\t\t\t\t\t\tstd::ignore = freerdp_abort_connect_context(sdl->context());')
        client.write_text(text)
    client.write_text(final_event_poll(client.read_text()))
    window = base / 'sdl_window.cpp'
    text = window.read_text()
    if not text.startswith(MARKER):
        text = replace(text, '#include "sdl_window.hpp"', '#include "sdl_window.hpp"\n#include "remotemanager-probe/SdlPerformanceProbe.h"')
        text = replace(text, '\tif (!SDL_UpdateTexture(_gdiTexture, &srcRect, pixels, surface->pitch))',
                       '\tconst auto probeBegin = PerformanceTrace::now();\n'
                       '\tif (!SDL_UpdateTexture(_gdiTexture, &srcRect, pixels, surface->pitch))')
        text = replace(text, '\t/* Render onto persistent render target to accumulate dirty rects */',
                       '\tsdlPerformance::trace.record({PerformanceStage::UploadPreparation, sdlPerformance::stamp(),\n'
                       '\t    probeBegin, PerformanceTrace::now(), quint64(srcRect.w) * quint64(srcRect.h) * quint64(bpp)});\n'
                       '\t/* Render onto persistent render target to accumulate dirty rects */')
        text = replace(text, '\tif (!SDL_RenderPresent(_renderer))\n\t\treturn;\n',
                       '\tstd::lock_guard<std::mutex> probeLock(sdlPerformance::mutex);\n'
                       '\tsdlPerformance::readback(_renderer, _gdiTexture);\n'
                       '\tconst auto probeBegin = PerformanceTrace::now();\n'
                       '\tif (!SDL_RenderPresent(_renderer))\n\t\treturn;\n'
                       '\tsdlPerformance::presented(_renderer, _window, probeBegin);\n')
        window.write_text(MARKER + text)
    # Remove earlier diagnostic-only forced sizing from already instrumented private sources.
    text = window.read_text()
    if 'SDL comparator window sizing failed' in text:
        text = replace(text, '\n\tif (sdlPerformance::enabled() && !SDL_SetWindowSize(_window, 1024, 768))\n'
                       '\t\tthrow std::runtime_error("SDL comparator window sizing failed");', '')
    if 'sdlPerformance::enabled() ? 1024' in text:
        text = replace(text, 'sdlPerformance::enabled() ? 1024 : static_cast<int>(std::ceil(static_cast<float>(_initialW) / pd))',
                       'static_cast<int>(std::ceil(static_cast<float>(_initialW) / pd))')
        text = replace(text, 'sdlPerformance::enabled() ? 768 : static_cast<int>(std::ceil(static_cast<float>(_initialH) / pd))',
                       'static_cast<int>(std::ceil(static_cast<float>(_initialH) / pd))')
    context_text, window_text = upload_batch(context.read_text(), text)
    context.write_text(context_text)
    window.write_text(window_text)
    cmake = base / 'CMakeLists.txt'
    text = cmake.read_text()
    if 'RemoteManager test-only comparator probe' not in text:
        text += '\n# RemoteManager test-only comparator probe\nfind_package(Qt6 6.12.0 REQUIRED COMPONENTS Core)\n'
        text += 'target_sources(${MODULE_NAME} PRIVATE remotemanager-probe/PerformanceTrace.cpp)\n'
        text += 'target_link_libraries(${MODULE_NAME} PRIVATE Qt6::Core)\n'
    if 'RM_COMPARATOR_RUNTIME_PATHS' not in text:
        text += 'set_property(TARGET ${MODULE_NAME} PROPERTY INSTALL_RPATH "${RM_COMPARATOR_RUNTIME_PATHS}")\n'
    cmake.write_text(text)
