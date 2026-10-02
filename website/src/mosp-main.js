import { createApp } from 'vue'
import '@fontsource-variable/manrope'
import '@fontsource-variable/oswald'
import '@fontsource/jetbrains-mono/latin-400.css'
import './style.css'
import './mosp.css'
import './motion.css'
import MospPage from './MospPage.vue'
import { installPageTransition } from './pageTransition'

createApp(MospPage).mount('#app')
installPageTransition()
