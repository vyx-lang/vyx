import { createApp } from 'vue'
import '@fontsource-variable/manrope'
import '@fontsource-variable/oswald'
import '@fontsource/jetbrains-mono/latin-400.css'
import './style.css'
import './motion.css'
import App from './App.vue'
import { installPageTransition } from './pageTransition'

createApp(App).mount('#app')
installPageTransition()
