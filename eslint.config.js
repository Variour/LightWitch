import js from '@eslint/js';
import globals from 'globals';
import html from 'eslint-plugin-html';

export default [
  { ignores: ['scripts/**'] },
  js.configs.recommended,
  {
    files: ['server/**/*.js'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'module',
      globals: globals.node,
    },
    rules: {
      eqeqeq: 'error',
      'prefer-const': 'error',
      'no-var': 'error',
    },
  },
  {
    files: ['data/index.html'],
    plugins: { html },
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'script',
      globals: globals.browser,
    },
    rules: {
      eqeqeq: 'error',
      'prefer-const': 'error',
      'no-var': 'error',
    },
  },
];
