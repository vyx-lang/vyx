package com.vyx.plugin

import com.intellij.openapi.components.*
import com.intellij.openapi.project.Project

@Service(Service.Level.PROJECT)
@State(name = "VyxSettings", storages = [Storage("vyx.xml")])
class VyxSettings : PersistentStateComponent<VyxSettings.State> {
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
        fun getInstance(project: Project): VyxSettings = project.getService(VyxSettings::class.java)
    }
}
