package com.vyx.plugin

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage

/**
 * IDE-wide toolchain defaults. The Settings page (Languages & Frameworks → Vyx)
 * is the single UI; on Apply it writes project settings and mirrors into here so
 * New Project wizard / new projects can default from the last configured paths.
 */
@Service(Service.Level.APP)
@State(name = "VyxApplicationSettings", storages = [Storage("vyx-tools.xml")])
class VyxApplicationSettings : PersistentStateComponent<VyxApplicationSettings.State> {
    data class State(
        var vyxcPath: String = "",
        var lspPath: String = "",
        var dapPath: String = "",
    )

    private var myState = State()

    override fun getState(): State = myState
    override fun loadState(state: State) { myState = state }

    var vyxcPath: String
        get() = myState.vyxcPath
        set(value) { myState.vyxcPath = value }

    var lspPath: String
        get() = myState.lspPath
        set(value) { myState.lspPath = value }

    var dapPath: String
        get() = myState.dapPath
        set(value) { myState.dapPath = value }

    companion object {
        fun getInstance(): VyxApplicationSettings =
            ApplicationManager.getApplication().getService(VyxApplicationSettings::class.java)
    }
}