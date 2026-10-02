import { createApp } from 'vue'
import '@fontsource-variable/manrope'
import '@fontsource-variable/oswald'
import '@fontsource/jetbrains-mono/latin-400.css'
import './style.css'
import './tutorial.css'
import './motion.css'
import TutorialPage from './TutorialPage.vue'
import { installPageTransition } from './pageTransition'

createApp(TutorialPage).mount('#app')
installPageTransition()
