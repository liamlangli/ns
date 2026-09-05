import SwiftUI
#if os(iOS) || os(visionOS)
import UIKit
#endif

@main
struct NSGeneratedApp: App {
    @State private var status = "Ready"
    @State private var started = false
    var body: some Scene {
        WindowGroup {
            #if os(iOS) || os(visionOS)
            NSGameView()
                .ignoresSafeArea()
                #if os(visionOS)
                .modifier(NSImmersiveControl())
                #endif
                .task { await ns_start_linked_project(status: $status, started: $started) }
            #else
            VStack(spacing: 12) {
                Text("NS").font(.largeTitle).bold()
                Text(status).multilineTextAlignment(.center)
            }
            .padding(24)
            .task { await ns_start_linked_project(status: $status, started: $started) }
            #endif
        }
        #if os(visionOS)
        ImmersiveSpace(id: "ns.immersive") {
            CompositorLayer(configuration: NSImmersiveConfiguration()) { renderer in
                NSImmersiveRenderer(renderer).start()
            }
            .onDisappear { ns_immersive_state(0) }
        }.immersionStyle(selection: .constant(.full), in: .full)
        #endif
    }
}

private func ns_start_linked_project(status: Binding<String>, started: Binding<Bool>) async {
    guard !started.wrappedValue else { return }
    started.wrappedValue = true
    status.wrappedValue = "Running…"
    let resourceRoot = Bundle.main.resourceURL?.path ?? Bundle.main.bundlePath
    status.wrappedValue = await Task.detached(priority: .userInitiated) {
        resourceRoot.withCString { String(cString: ns_run_linked_project($0)) }
    }.value
}

#if os(iOS) || os(visionOS)
struct NSGameView: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .black
        view.isMultipleTouchEnabled = true
        view_ios_set_host_view(Unmanaged.passUnretained(view).toOpaque())
        return view
    }
    func updateUIView(_ uiView: UIView, context: Context) {
        view_ios_set_host_view(Unmanaged.passUnretained(uiView).toOpaque())
    }
}
#endif

#if os(visionOS)
import ARKit
import CompositorServices
import Metal
import simd

struct NSImmersiveControl: ViewModifier {
    @State private var isOpen = false
    @Environment(\.openImmersiveSpace) private var openSpace
    @Environment(\.dismissImmersiveSpace) private var dismissSpace
    func body(content: Content) -> some View {
        content.task {
            view_immersive_host_support(1)
            while !Task.isCancelled {
                let state = view_immersive_status()
                if view_immersive_host_requested() != 0 && state <= 0 {
                    ns_immersive_state(1)
                    switch await openSpace(id: "ns.immersive") {
                    case .opened: isOpen = true // The renderer publishes active after tracking starts.
                    case .userCancelled, .error: ns_immersive_state(-1)
                    @unknown default: ns_immersive_state(-1)
                    }
                } else if view_immersive_host_requested() == 0 && (state == 2 || state == 1) {
                    ns_immersive_state(3)
                    await dismissSpace()
                    isOpen = false
                    ns_immersive_state(0)
                }
                try? await Task.sleep(for: .milliseconds(50))
            }
        }
    }
}

struct NSImmersiveConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // Foveation needs rasterization-rate maps this renderer does not apply.
        configuration.isFoveationEnabled = false
        configuration.drawableRenderContextRasterSampleCount = 1
        let stencilFormats = capabilities.drawableRenderContextSupportedStencilFormats
        if stencilFormats.contains(.stencil8) {
            configuration.drawableRenderContextStencilFormat = .stencil8
        } else if stencilFormats.contains(.depth32Float_stencil8) {
            configuration.drawableRenderContextStencilFormat = .depth32Float_stencil8
        }
        let colors = capabilities.supportedColorFormats(options: [])
        if let format = ns_pick_pixel_format([.bgra8Unorm, .bgra8Unorm_srgb, .rgba16Float], from: colors) {
            configuration.colorFormat = format
        }
        // Combined depth-stencil is only required when the portal mask cannot use a
        // separate stencil8 target. Keep depth32Float otherwise so game pipelines
        // do not have to declare a stencil attachment.
        let depthPreferred: [MTLPixelFormat] =
            configuration.drawableRenderContextStencilFormat == .depth32Float_stencil8
            ? [.depth32Float_stencil8, .depth32Float]
            : [.depth32Float, .depth32Float_stencil8]
        if let format = ns_pick_pixel_format(depthPreferred, from: capabilities.supportedDepthFormats) {
            configuration.depthFormat = format
        }
        let layouts = capabilities.supportedLayouts(options: [])
        if layouts.contains(.layered) {
            configuration.layout = .layered
        } else if layouts.contains(.dedicated) {
            configuration.layout = .dedicated
        }
    }
}

private func ns_pick_pixel_format(_ preferred: [MTLPixelFormat], from supported: [MTLPixelFormat]) -> MTLPixelFormat? {
    for format in preferred {
        if supported.contains(format) { return format }
    }
    return supported.first
}

// One serial render loop. Application callbacks and pointer publication run on
// the main thread, shared with MTKView, so the ns VM never runs concurrently.
final class NSImmersiveRenderer: @unchecked Sendable {
    let layer: LayerRenderer
    let session = ARKitSession()
    let tracking = WorldTrackingProvider()
    var inputView = matrix_identity_float4x4
    var inputProjection = matrix_identity_float4x4
    var pointerDown = false
    let trackingSupported = WorldTrackingProvider.isSupported
    var portalStencil: (any MTLTexture)?
    init(_ layer: LayerRenderer) { self.layer = layer }
    func start() {
        Task { @MainActor [self] in
            if trackingSupported {
                do { try await session.run([tracking]) }
                catch { ns_immersive_state(-1); return }
            }
            layer.onSpatialEvent = { [weak self] events in
                guard let self else { return }
                for event in events {
                    if event.phase == .ended || event.phase == .cancelled {
                        ns_immersive_pointer(0.5, 0.5, 2)
                        pointerDown = false
                    } else if let ray = event.selectionRay {
                        let direction = SIMD4<Float>(Float(ray.direction.x), Float(ray.direction.y), Float(ray.direction.z), 0)
                        let local = inputView * direction
                        guard local.z < -0.001 else { continue }
                        let projected = inputProjection * local
                        let x = Double(projected.x / projected.w + 1) * 0.5
                        let y = Double(1 - projected.y / projected.w) * 0.5
                        ns_immersive_pointer(x, y, pointerDown ? 1 : 0)
                        pointerDown = true
                    }
                }
            }
            ns_immersive_state(2)
            Thread { [self] in renderLoop() }.start()
        }
    }
    func renderLoop() {
        guard let queue = layer.device.makeCommandQueue() else {
            DispatchQueue.main.async { ns_immersive_state(-1) }
            return
        }
        queue.label = "immersive stereo frames"
        let clock = LayerRenderer.Clock()
        var lastTracked = CACurrentMediaTime()
        while layer.state != .invalidated {
            if layer.state == .paused { layer.waitUntilRunning(); continue }
            autoreleasepool {
                guard let frame = layer.queryNextFrame() else { return }
                frame.startUpdate()
                frame.endUpdate()
                // A paused or invalidated layer yields no timing; the frame is already
                // cancelled and must not be submitted.
                guard let timing = frame.predictTiming() else { return }
                clock.wait(until: timing.optimalInputTime)
                if layer.state != .running { return }
                // An empty list means the compositor cancelled this frame. Accessing
                // it after that, including endSubmission, is a client error.
                let drawables = frame.queryDrawables()
                guard !drawables.isEmpty, let buffer = queue.makeCommandBuffer() else { return }
                var head = matrix_identity_float4x4
                if trackingSupported {
                    var anchor: DeviceAnchor?
                    for drawable in drawables {
                        let duration = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime).components
                        let time = Double(duration.seconds) + Double(duration.attoseconds) / 1e18
                        if let found = tracking.queryDeviceAnchor(atTimestamp: time), found.isTracked {
                            anchor = found
                            break
                        }
                    }
                    guard let anchor else {
                        // Never reuse a stale pose while the user's head is moving.
                        if CACurrentMediaTime() - lastTracked > 3 {
                            DispatchQueue.main.async { _ = view_immersive_request(0) }
                        }
                        return
                    }
                    lastTracked = CACurrentMediaTime()
                    head = anchor.originFromAnchorTransform
                    for drawable in drawables { drawable.deviceAnchor = anchor }
                }
                if layer.state != .running { return }
                frame.startSubmission()
                buffer.label = "immersive stereo frame"
                let gameIndex = drawables.firstIndex(where: { $0.target == .builtIn }) ?? 0
                for (index, drawable) in drawables.enumerated() {
                    if index == gameIndex {
                        ns_render_drawable_eyes(drawable, commandBuffer: buffer, head: head)
                    }
                    ns_present_drawable(drawable, commandBuffer: buffer)
                }
                buffer.commit()
                frame.endSubmission()
                // CPU physics reads the preceding frame's shared GPU output.
                // Finish both eyes before that mirror or transient ring is reused.
                buffer.waitUntilCompleted()
                DispatchQueue.main.sync { ns_immersive_complete() }
                if buffer.status == .error {
                    DispatchQueue.main.async { _ = view_immersive_request(0) }
                }
            }
        }
        session.stop()
        DispatchQueue.main.async { ns_immersive_state(0) }
    }

    func ns_render_drawable_eyes(_ drawable: LayerRenderer.Drawable, commandBuffer: MTLCommandBuffer, head: simd_float4x4) {
        for (eye, view) in drawable.views.enumerated() {
            let transform = head * view.transform
            let projection = drawable.computeProjection(viewIndex: eye)
            let pose = [transform, projection, head].flatMap { matrix in
                (0..<4).flatMap { column in (0..<4).map { matrix[column][$0] } }
            }
            let color = drawable.colorTextures[view.textureMap.textureIndex]
            let depth = drawable.depthTextures[view.textureMap.textureIndex]
            let slice = Int32(view.textureMap.sliceIndex)
            DispatchQueue.main.sync {
                if eye == 0 {
                    inputView = transform.inverse
                    inputProjection = projection
                }
                guard view_immersive_status() == 2 else { return }
                pose.withUnsafeBufferPointer {
                    ns_immersive_render_eye(Unmanaged.passUnretained(commandBuffer as AnyObject).toOpaque(),
                        Unmanaged.passUnretained(color as AnyObject).toOpaque(),
                        Unmanaged.passUnretained(depth as AnyObject).toOpaque(), Int32(eye), $0.baseAddress!, slice)
                }
            }
        }
    }

    // Progressive immersion presents through the drawable render context, not
    // encodePresent alone. Scene content is already in the color/depth
    // textures; this encoder only applies the portal mask and compositor effects.
    func ns_present_drawable(_ drawable: LayerRenderer.Drawable, commandBuffer: MTLCommandBuffer) {
        let renderContext = drawable.addRenderContext(commandBuffer: commandBuffer)
        guard let color = drawable.colorTextures.first else { return }
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = color
        descriptor.colorAttachments[0].loadAction = .load
        descriptor.colorAttachments[0].storeAction = .store
        if let depth = drawable.depthTextures.first {
            descriptor.depthAttachment.texture = depth
            descriptor.depthAttachment.loadAction = .load
            descriptor.depthAttachment.storeAction = .store
            if depth.pixelFormat == .depth32Float_stencil8 {
                descriptor.stencilAttachment.texture = depth
            }
        }
        if descriptor.stencilAttachment.texture == nil {
            let views = max(drawable.views.count, 1)
            if portalStencil == nil || portalStencil!.width != color.width ||
                portalStencil!.height != color.height || portalStencil!.arrayLength != views {
                let stencil = MTLTextureDescriptor()
                stencil.textureType = views > 1 || color.textureType == .type2DArray ? .type2DArray : .type2D
                stencil.pixelFormat = .stencil8
                stencil.width = color.width
                stencil.height = color.height
                stencil.arrayLength = views
                stencil.usage = .renderTarget
                stencil.storageMode = .memoryless
                portalStencil = color.device.makeTexture(descriptor: stencil)
                if portalStencil == nil {
                    stencil.storageMode = .private
                    portalStencil = color.device.makeTexture(descriptor: stencil)
                }
                portalStencil?.label = "immersive portal stencil"
            }
            descriptor.stencilAttachment.texture = portalStencil
        }
        if descriptor.stencilAttachment.texture != nil {
            descriptor.stencilAttachment.loadAction = .clear
            descriptor.stencilAttachment.clearStencil = 0
            descriptor.stencilAttachment.storeAction = .dontCare
        }
        if layer.configuration.layout == .layered {
            descriptor.renderTargetArrayLength = drawable.views.count
        }
        descriptor.rasterizationRateMap = drawable.rasterizationRateMaps.first
        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }
        encoder.label = "immersive present"
        if descriptor.stencilAttachment.texture != nil {
            renderContext.drawMaskOnStencilAttachment(commandEncoder: encoder, value: 200)
        }
        renderContext.endEncoding(commandEncoder: encoder)
        drawable.encodePresent(commandBuffer: commandBuffer)
    }
}
#endif
